/*
 *  vdpau_video.c - VDPAU backend for VA API
 *
 *  vdpau-video (C) 2009 Splitted-Desktop Systems
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

#define _GNU_SOURCE 1
#include "sysdeps.h"
#include "vdpau_video.h"
#include <va/va_backend.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include <dlfcn.h>

#define DEBUG 1
#include "debug.h"

#define ASSERT assert

/* Define to 1 if we want this VDPAU backend to handle H.264 DPB itself and not
   strictly replicate VAPictureParameterBufferH264.ReferenceFrames[]. */
#define VDPAU_VIDEO_DPB 1

/* Define wait delay (in microseconds) for vaSyncSurface() implementation
   with polling. */
#define VDPAU_SYNC_DELAY 5000

#define INIT_DRIVER_DATA \
        struct vdpau_driver_data *driver_data = (struct vdpau_driver_data *)ctx->pDriverData

#define CONFIG(id) \
        ((object_config_p)object_heap_lookup(&driver_data->config_heap, id))
#define CONTEXT(id) \
        ((object_context_p)object_heap_lookup(&driver_data->context_heap, id))
#define SURFACE(id) \
        ((object_surface_p)object_heap_lookup(&driver_data->surface_heap, id))
#define BUFFER(id) \
        ((object_buffer_p)object_heap_lookup(&driver_data->buffer_heap, id))
#define OUTPUT(id) \
        ((object_output_p)object_heap_lookup(&driver_data->output_heap, id))
#define IMAGE(id) \
        ((object_image_p)object_heap_lookup(&driver_data->image_heap, id))

#define CONFIG_ID_OFFSET        0x01000000
#define CONTEXT_ID_OFFSET       0x02000000
#define SURFACE_ID_OFFSET       0x03000000
#define BUFFER_ID_OFFSET        0x04000000
#define OUTPUT_ID_OFFSET        0x05000000
#define IMAGE_ID_OFFSET         0x06000000


/* ====================================================================== */
/* === Helpers                                                        === */
/* ====================================================================== */

#ifndef VA_INVALID_SURFACE
#define VA_INVALID_SURFACE 0xffffffff
#endif

typedef struct {
    VdpImageFormatType type;
    uint32_t format;
    VAImageFormat va_format;
} vdpau_image_format_map_t;

static const vdpau_image_format_map_t vdpau_image_formats_map[] = {
#define DEF(TYPE, FORMAT) \
    VDP_IMAGE_FORMAT_TYPE_##TYPE, VDP_##TYPE##_FORMAT_##FORMAT
#define DEF_YUV(TYPE, FORMAT, FOURCC, ENDIAN, BPP) \
    { DEF(TYPE, FORMAT), { VA_FOURCC FOURCC, VA_##ENDIAN##_FIRST, BPP, } }
#define DEF_RGB(TYPE, FORMAT, FOURCC, ENDIAN, BPP, DEPTH, R,G,B,A) \
    { DEF(TYPE, FORMAT), { VA_FOURCC FOURCC, VA_##ENDIAN##_FIRST, BPP, DEPTH, R,G,B,A } }
    DEF_YUV(YCBCR, NV12,        ('N','V','1','2'), LSB, 12),
    DEF_YUV(YCBCR, YV12,        ('Y','V','1','2'), LSB, 12),
    DEF_YUV(YCBCR, UYVY,        ('U','Y','V','Y'), LSB, 16),
    DEF_YUV(YCBCR, YUYV,        ('Y','U','Y','V'), LSB, 16),
    DEF_YUV(YCBCR, V8U8Y8A8,    ('A','Y','U','V'), LSB, 32),
#ifdef WORDS_BIGENDIAN
    DEF_RGB(RGBA, B8G8R8A8,     ('R','G','B','A'), MSB, 32,
            32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000),
    DEF_RGB(RGBA, R8G8B8A8,     ('R','G','B','A'), MSB, 32,
            32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000),
#else
    DEF_RGB(RGBA, B8G8R8A8,     ('R','G','B','A'), LSB, 32,
            32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000),
    DEF_RGB(RGBA, R8G8B8A8,     ('R','G','B','A'), LSB, 32,
            32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000),
#endif
#undef DEF_RGB
#undef DEF_YUV
#undef DEF
};

// Get current value of microsecond timer
static uint64_t get_ticks_usec(void)
{
#ifdef HAVE_CLOCK_GETTIME
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
#else
    struct timeval t;
    gettimeofday(&t, NULL);
    return (uint64_t)t.tv_sec * 1000000 + t.tv_usec;
#endif
}

#if defined(__linux__)
// Linux select() changes its timeout parameter upon return to contain
// the remaining time. Most other unixen leave it unchanged or undefined.
#define SELECT_SETS_REMAINING
#elif defined(__FreeBSD__) || defined(__sun__) || (defined(__MACH__) && defined(__APPLE__))
#define USE_NANOSLEEP
#elif defined(HAVE_PTHREADS) && defined(sgi)
// SGI pthreads has a bug when using pthreads+signals+nanosleep,
// so instead of using nanosleep, wait on a CV which is never signalled.
#include <pthread.h>
#define USE_COND_TIMEDWAIT
#endif

// Wait for the specified amount of microseconds
static void delay_usec(unsigned int usec)
{
    int was_error;

#if defined(USE_NANOSLEEP)
    struct timespec elapsed, tv;
#elif defined(USE_COND_TIMEDWAIT)
    // Use a local mutex and cv, so threads remain independent
    pthread_cond_t delay_cond = PTHREAD_COND_INITIALIZER;
    pthread_mutex_t delay_mutex = PTHREAD_MUTEX_INITIALIZER;
    struct timespec elapsed;
    uint64_t future;
#else
    struct timeval tv;
#ifndef SELECT_SETS_REMAINING
    uint64_t then, now, elapsed;
#endif
#endif

    // Set the timeout interval - Linux only needs to do this once
#if defined(SELECT_SETS_REMAINING)
    tv.tv_sec = 0;
    tv.tv_usec = usec;
#elif defined(USE_NANOSLEEP)
    elapsed.tv_sec = 0;
    elapsed.tv_nsec = usec * 1000;
#elif defined(USE_COND_TIMEDWAIT)
    future = get_ticks_usec() + usec;
    elapsed.tv_sec = future / 1000000;
    elapsed.tv_nsec = (future % 1000000) * 1000;
#else
    then = get_ticks_usec();
#endif

    do {
        errno = 0;
#if defined(USE_NANOSLEEP)
        tv.tv_sec = elapsed.tv_sec;
        tv.tv_nsec = elapsed.tv_nsec;
        was_error = nanosleep(&tv, &elapsed);
#elif defined(USE_COND_TIMEDWAIT)
        was_error = pthread_mutex_lock(&delay_mutex);
        was_error = pthread_cond_timedwait(&delay_cond, &delay_mutex, &elapsed);
        was_error = pthread_mutex_unlock(&delay_mutex);
#else
#ifndef SELECT_SETS_REMAINING
        // Calculate the time interval left (in case of interrupt)
        now = get_ticks_usec();
        elapsed = now - then;
        then = now;
        if (elapsed >= usec)
            break;
        usec -= elapsed;
        tv.tv_sec = 0;
        tv.tv_usec = usec;
#endif
        was_error = select(0, NULL, NULL, NULL, &tv);
#endif
    } while (was_error && (errno == EINTR));
}

// X error trap
static int x11_error_code = 0;
static int (*old_error_handler)(Display *, XErrorEvent *);

static int error_handler(Display *dpy, XErrorEvent *error)
{
    x11_error_code = error->error_code;
    return 0;
}

void x11_trap_errors(void)
{
    x11_error_code    = 0;
    old_error_handler = XSetErrorHandler(error_handler);
}

int x11_untrap_errors(void)
{
    XSetErrorHandler(old_error_handler);
    return x11_error_code;
}

// Returns X drawable dimensions
static void get_drawable_size(Display  *display,
                              Drawable  drawable,
                              uint32_t *pwidth,
                              uint32_t *pheight)
{
    Window rootwin;
    int x, y;
    unsigned int width, height, border_width, depth;
    XGetGeometry(display, drawable, &rootwin,
                 &x, &y, &width, &height, &border_width, &depth);
    if (pwidth)
        *pwidth = width;
    if (pheight)
        *pheight = height;
}

// Returns string representation of VABufferType
static const char *string_of_VABufferType(VABufferType type)
{
    const char *str = NULL;
    switch (type) {
#define _(X) case X: str = #X; break
        _(VAPictureParameterBufferType);
        _(VAIQMatrixBufferType);
        _(VABitPlaneBufferType);
        _(VASliceGroupMapBufferType);
        _(VASliceParameterBufferType);
        _(VASliceDataBufferType);
        _(VAMacroblockParameterBufferType);
        _(VAResidualDataBufferType);
        _(VADeblockingParameterBufferType);
        _(VAImageBufferType);
#if VA_CHECK_VERSION(0,30,0)
        _(VAProtectedSliceDataBufferType);
        _(VAEncCodedBufferType);
        _(VAEncSequenceParameterBufferType);
        _(VAEncPictureParameterBufferType);
        _(VAEncSliceParameterBufferType);
        _(VAEncH264VUIBufferType);
        _(VAEncH264SEIBufferType);
#endif
#undef _
    }
    return str;
}

// Returns string representation of VdpCodec
static const char *string_of_VdpCodec(VdpCodec codec)
{
    const char *str = NULL;
    switch (codec) {
#define _(X) case VDP_CODEC_##X: str = #X; break
        _(MPEG1);
        _(MPEG2);
        _(MPEG4);
        _(H264);
        _(VC1);
#undef _
    }
    return str;
}

// Translates VdpDecoderProfile to VdpCodec
static VdpCodec get_VdpCodec(VdpDecoderProfile profile)
{
    switch (profile) {
    case VDP_DECODER_PROFILE_MPEG1:
        return VDP_CODEC_MPEG1;
    case VDP_DECODER_PROFILE_MPEG2_SIMPLE:
    case VDP_DECODER_PROFILE_MPEG2_MAIN:
        return VDP_CODEC_MPEG2;
    case VDP_DECODER_PROFILE_H264_BASELINE:
    case VDP_DECODER_PROFILE_H264_MAIN:
    case VDP_DECODER_PROFILE_H264_HIGH:
        return VDP_CODEC_H264;
    case VDP_DECODER_PROFILE_VC1_SIMPLE:
    case VDP_DECODER_PROFILE_VC1_MAIN:
    case VDP_DECODER_PROFILE_VC1_ADVANCED:
        return VDP_CODEC_VC1;
    }
    ASSERT(profile);
    return 0;
}

// Computes value for VdpDecoderCreate()::max_references parameter
static int get_VdpDecoder_max_references(VdpDecoderProfile profile,
                                         uint32_t          width,
                                         uint32_t          height)
{
    int max_references = 2;
    switch (profile) {
    case VDP_DECODER_PROFILE_H264_MAIN:
    case VDP_DECODER_PROFILE_H264_HIGH:
    {
        /* level 4.1 limits */
        uint32_t aligned_width  = (width + 15) & -16;
        uint32_t aligned_height = (height + 15) & -16;
        uint32_t surface_size   = (aligned_width * aligned_height * 3) / 2;
        if ((max_references = (12 * 1024 * 1024) / surface_size) > 16)
            max_references = 16;
        break;
    }
    }
    return max_references;
}

// Translates VA API chroma format to VdpChromaType
static VdpChromaType get_VdpChromaType(int format)
{
    switch (format) {
    case VA_RT_FORMAT_YUV420: return VDP_CHROMA_TYPE_420;
    case VA_RT_FORMAT_YUV422: return VDP_CHROMA_TYPE_422;
    case VA_RT_FORMAT_YUV444: return VDP_CHROMA_TYPE_444;
    }
    ASSERT(format);
    return (VdpChromaType)-1;
}

// Translates VAProfile to VdpDecoderProfile
static VdpDecoderProfile get_VdpDecoderProfile(VAProfile profile)
{
    switch (profile) {
    case VAProfileMPEG2Simple:  return VDP_DECODER_PROFILE_MPEG2_SIMPLE;
    case VAProfileMPEG2Main:    return VDP_DECODER_PROFILE_MPEG2_MAIN;
    case VAProfileH264Baseline: return VDP_DECODER_PROFILE_H264_BASELINE;
    case VAProfileH264Main:     return VDP_DECODER_PROFILE_H264_MAIN;
    case VAProfileH264High:     return VDP_DECODER_PROFILE_H264_HIGH;
    case VAProfileVC1Simple:    return VDP_DECODER_PROFILE_VC1_SIMPLE;
    case VAProfileVC1Main:      return VDP_DECODER_PROFILE_VC1_MAIN;
    case VAProfileVC1Advanced:  return VDP_DECODER_PROFILE_VC1_ADVANCED;
    default:                    break;
    }
    ASSERT(profile);
    return (VdpDecoderProfile)-1;
}

// Translates VA API image format to VdpYCbCrFormat
static VdpYCbCrFormat get_VdpYCbCrFormat(const VAImageFormat *image_format)
{
    int i;
    for (i = 0; i < ARRAY_ELEMS(vdpau_image_formats_map); i++) {
        const vdpau_image_format_map_t * const m = &vdpau_image_formats_map[i];
        if (m->type != VDP_IMAGE_FORMAT_TYPE_YCBCR)
            continue;
        if (m->va_format.fourcc == image_format->fourcc)
            return m->format;
    }
    ASSERT(image_format->fourcc);
    return (VdpYCbCrFormat)-1;
}

// Translates VA API image format to VdpRGBAFormat
static VdpRGBAFormat get_VdpRGBAFormat(const VAImageFormat *image_format)
{
    int i;
    for (i = 0; i < ARRAY_ELEMS(vdpau_image_formats_map); i++) {
        const vdpau_image_format_map_t * const m = &vdpau_image_formats_map[i];
        if (m->type != VDP_IMAGE_FORMAT_TYPE_RGBA)
            continue;
        if (m->va_format.fourcc == image_format->fourcc &&
            m->va_format.byte_order == image_format->byte_order &&
            m->va_format.red_mask == image_format->red_mask &&
            m->va_format.green_mask == image_format->green_mask &&
            m->va_format.blue_mask == image_format->blue_mask)
            return m->format;
    }
    return (VdpRGBAFormat)-1;
}

// Returns 1 if we want to handle H.264 DPB ourselves
static int g_vdpau_video_dpb = -1;

static int vdpau_video_dpb_1(void)
{
    const char *vdpau_video_dpb_str = getenv("VDPAU_VIDEO_DPB");
    if (vdpau_video_dpb_str) {
        if (strcmp(vdpau_video_dpb_str, "1") == 0 ||
            strcmp(vdpau_video_dpb_str, "yes") == 0)
            g_vdpau_video_dpb = 1;
        else if (strcmp(vdpau_video_dpb_str, "0") == 0 ||
                 strcmp(vdpau_video_dpb_str, "no") == 0)
            g_vdpau_video_dpb = 0;
    }
    if (g_vdpau_video_dpb < 0)
        g_vdpau_video_dpb = VDPAU_VIDEO_DPB;
    return g_vdpau_video_dpb;
}

static inline int vdpau_video_dpb(void)
{
    if (g_vdpau_video_dpb < 0)
        g_vdpau_video_dpb = vdpau_video_dpb_1();
    return g_vdpau_video_dpb;
}


/* ====================================================================== */
/* === OpenGL Gate and Helpers                                        === */
/* ====================================================================== */

#if USE_GLX
typedef void (*GLFuncPtr)(void);
typedef GLFuncPtr (*GLXGetProcAddressProc)(const char *);

static GLFuncPtr get_proc_address_default(const char *name)
{
    return NULL;
}

static GLXGetProcAddressProc get_proc_address_func(void)
{
    GLXGetProcAddressProc get_proc_func;

    dlerror();
    get_proc_func = (GLXGetProcAddressProc)
        dlsym(RTLD_NEXT, "glXGetProcAddress");
    if (dlerror() == NULL)
        return get_proc_func;

    get_proc_func = (GLXGetProcAddressProc)
        dlsym(RTLD_NEXT, "glXGetProcAddressARB");
    if (dlerror() == NULL)
        return get_proc_func;

    return get_proc_address_default;
}

static inline GLFuncPtr get_proc_address(const char *name)
{
    static GLXGetProcAddressProc get_proc_func = NULL;
    if (get_proc_func == NULL)
        get_proc_func = get_proc_address_func();
    return get_proc_func(name);
}

static int check_extension(const char *name, const char *ext)
{
    const char *end;
    int name_len, n;

    if (name == NULL || ext == NULL)
        return 0;

    end = ext + strlen(ext);
    name_len = strlen(name);
    while (ext < end) {
        n = strcspn(ext, " ");
        if (n == name_len && strncmp(name, ext, n) == 0)
            return 1;
        ext += (n + 1);
    }
    return 0;
}

static int gl_check_extensions(vdpau_driver_data_t *driver_data)
{
    VADriverContextP const ctx = driver_data->va_context;
    const char *gl_extensions;
    const char *glx_extensions;

    gl_extensions  = (const char *)glGetString(GL_EXTENSIONS);
    glx_extensions = glXQueryExtensionsString(ctx->x11_dpy, ctx->x11_screen);

    if (!check_extension("GL_ARB_texture_non_power_of_two", gl_extensions))
        return -1;
    if (!check_extension("GLX_EXT_texture_from_pixmap", glx_extensions))
        return -1;
    if (!check_extension("GL_ARB_framebuffer_object", gl_extensions) &&
        !check_extension("GL_EXT_framebuffer_object", gl_extensions))
        return -1;
    return 0;
}

static int gl_load_funcs(vdpau_driver_data_t *driver_data)
{
    opengl_data_t * const gl_data = &driver_data->gl_data;

    gl_data->glx_bind_tex_image = (PFNGLXBINDTEXIMAGEEXTPROC)
        get_proc_address("glXBindTexImageEXT");
    if (gl_data->glx_bind_tex_image == NULL)
        return -1;
    gl_data->glx_release_tex_image = (PFNGLXRELEASETEXIMAGEEXTPROC)
        get_proc_address("glXReleaseTexImageEXT");
    if (gl_data->glx_release_tex_image == NULL)
        return -1;
    gl_data->gl_gen_framebuffers = (PFNGLGENFRAMEBUFFERSEXTPROC)
        get_proc_address("glGenFramebuffersEXT");
    if (gl_data->gl_gen_framebuffers == NULL)
        return -1;
    gl_data->gl_delete_framebuffers = (PFNGLDELETEFRAMEBUFFERSEXTPROC)
        get_proc_address("glDeleteFramebuffersEXT");
    if (gl_data->gl_delete_framebuffers == NULL)
        return -1;
    gl_data->gl_bind_framebuffer = (PFNGLBINDFRAMEBUFFEREXTPROC)
        get_proc_address("glBindFramebufferEXT");
    if (gl_data->gl_bind_framebuffer == NULL)
        return -1;
    gl_data->gl_gen_renderbuffers = (PFNGLGENRENDERBUFFERSEXTPROC)
        get_proc_address("glGenRenderbuffersEXT");
    if (gl_data->gl_gen_renderbuffers == NULL)
        return -1;
    gl_data->gl_delete_renderbuffers = (PFNGLDELETERENDERBUFFERSEXTPROC)
        get_proc_address("glDeleteRenderbuffersEXT");
    if (gl_data->gl_delete_renderbuffers == NULL)
        return -1;
    gl_data->gl_bind_renderbuffer = (PFNGLBINDRENDERBUFFEREXTPROC)
        get_proc_address("glBindRenderbufferEXT");
    if (gl_data->gl_bind_renderbuffer == NULL)
        return -1;
    gl_data->gl_renderbuffer_storage = (PFNGLRENDERBUFFERSTORAGEEXTPROC)
        get_proc_address("glRenderbufferStorageEXT");
    if (gl_data->gl_renderbuffer_storage == NULL)
        return -1;
    gl_data->gl_framebuffer_renderbuffer = (PFNGLFRAMEBUFFERRENDERBUFFEREXTPROC)
        get_proc_address("glFramebufferRenderbufferEXT");
    if (gl_data->gl_framebuffer_renderbuffer == NULL)
        return -1;
    gl_data->gl_framebuffer_texture_2d = (PFNGLFRAMEBUFFERTEXTURE2DEXTPROC)
        get_proc_address("glFramebufferTexture2DEXT");
    if (gl_data->gl_framebuffer_texture_2d == NULL)
        return -1;
    gl_data->gl_check_framebuffer_status = (PFNGLCHECKFRAMEBUFFERSTATUSEXTPROC)
        get_proc_address("glCheckFramebufferStatusEXT");
    if (gl_data->gl_check_framebuffer_status == NULL)
        return -1;
    return 0;
}

static const char *gl_error_string(GLenum error)
{
    static const struct {
        GLenum val;
        const char *str;
    }
    gl_errors[] = {
        { GL_NO_ERROR,          "no error" },
        { GL_INVALID_ENUM,      "invalid enumerant" },
        { GL_INVALID_VALUE,     "invalid value" },
        { GL_INVALID_OPERATION, "invalid operation" },
        { GL_STACK_OVERFLOW,    "stack overflow" },
        { GL_STACK_UNDERFLOW,   "stack underflow" },
        { GL_OUT_OF_MEMORY,     "out of memory" },
#ifdef GL_INVALID_FRAMEBUFFER_OPERATION_EXT
        { GL_INVALID_FRAMEBUFFER_OPERATION_EXT, "invalid framebuffer operation" },
#endif
        { ~0, NULL }
    };

    int i;
    for (i = 0; gl_errors[i].str; i++) {
        if (gl_errors[i].val == error)
            return gl_errors[i].str;
    }
    return "unknown";
}

static int gl_do_check_error(int report)
{
    GLenum error;
    int is_error = 0;
#if DEBUG
    while ((error = glGetError()) != GL_NO_ERROR) {
        if (report)
            vdpau_error_message("glError: %s caught\n", gl_error_string(error));
        is_error = 1;
    }
#endif
    return is_error;
}

static inline void gl_purge_errors(void)
{
    gl_do_check_error(0);
}

static inline int gl_check_error(void)
{
    return gl_do_check_error(1);
}

static int gl_get_current_color(float color[4])
{
    gl_purge_errors();
    glGetFloatv(GL_CURRENT_COLOR, color);
    if (gl_check_error())
        return -1;
    return 0;
}

static int gl_get_param(GLenum param, unsigned int *pval)
{
    GLint val;

    gl_purge_errors();
    glGetIntegerv(param, &val);
    if (gl_check_error())
        return -1;
    if (pval)
        *pval = val;
    return 0;
}

static int gl_get_texture_param(GLenum param, unsigned int *pval)
{
    GLint val;

    gl_purge_errors();
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, param, &val);
    if (gl_check_error())
        return -1;
    if (pval)
        *pval = val;
    return 0;
}

static Pixmap gen_pixmap(vdpau_driver_data_t *driver_data,
                         unsigned int         width,
                         unsigned int         height)
{
    VADriverContextP const ctx = driver_data->va_context;
    Window root_window;
    XWindowAttributes wattr;

    root_window = RootWindow(ctx->x11_dpy, ctx->x11_screen);
    XGetWindowAttributes(ctx->x11_dpy, root_window, &wattr);
    return XCreatePixmap(ctx->x11_dpy, root_window, width, height, wattr.depth);
}

static GLXPixmap gen_glx_pixmap(vdpau_driver_data_t *driver_data, Pixmap pixmap)
{
    VADriverContextP const ctx = driver_data->va_context;
    GLXFBConfig *fbconfig = NULL;
    GLXPixmap glx_pixmap  = None;
    int *attrib, n_fbconfig_attribs;
    Window root_window;
    int x, y;
    unsigned int width, height, border_width, depth;
    int status;

    if (pixmap == None)
        return -1;

    x11_trap_errors();
    status = XGetGeometry(ctx->x11_dpy,
                          (Drawable)pixmap,
                          &root_window,
                          &x,
                          &y,
                          &width,
                          &height,
                          &border_width,
                          &depth);
    if (x11_untrap_errors() != 0 || status == 0)
        return -1;
    if (depth != 24 && depth != 32)
        return -1;

    int fbconfig_attribs[32] = {
        GLX_DRAWABLE_TYPE,      GLX_PIXMAP_BIT,
        GLX_DOUBLEBUFFER,       GL_TRUE,
        GLX_RENDER_TYPE,        GLX_RGBA_BIT,
        GLX_X_RENDERABLE,       GL_TRUE,
        GLX_Y_INVERTED_EXT,     GL_TRUE,
        GLX_RED_SIZE,           8,
        GLX_GREEN_SIZE,         8,
        GLX_BLUE_SIZE,          8,
        GL_NONE,
    };
    for (attrib = fbconfig_attribs; *attrib != GL_NONE; attrib += 2)
        ;
    *attrib++ = GLX_DEPTH_SIZE;                         *attrib++ = depth;
    if (depth == 32) {
        *attrib++ = GLX_ALPHA_SIZE;                     *attrib++ = 8;
        *attrib++ = GLX_BIND_TO_TEXTURE_RGBA_EXT;       *attrib++ = GL_TRUE;
    }
    else {
        *attrib++ = GLX_BIND_TO_TEXTURE_RGB_EXT;        *attrib++ = GL_TRUE;
    }
    *attrib++ = GL_NONE;

    fbconfig = glXChooseFBConfig(ctx->x11_dpy, ctx->x11_screen, fbconfig_attribs, &n_fbconfig_attribs);
    if (fbconfig == NULL)
        return -1;

    int pixmap_attribs[10] = {
        GLX_TEXTURE_TARGET_EXT, GLX_TEXTURE_2D_EXT,
        GLX_MIPMAP_TEXTURE_EXT, GL_FALSE,
        GL_NONE,
    };
    for (attrib = pixmap_attribs; *attrib != GL_NONE; attrib += 2)
        ;
    *attrib++ = GLX_TEXTURE_FORMAT_EXT;
    if (depth == 32)
        *attrib++ = GLX_TEXTURE_FORMAT_RGBA_EXT;
    else
        *attrib++ = GLX_TEXTURE_FORMAT_RGB_EXT;
    *attrib++ = GL_NONE;

    x11_trap_errors();
    glx_pixmap = glXCreatePixmap(ctx->x11_dpy,
                                 fbconfig[0],
                                 pixmap,
                                 pixmap_attribs);
    free(fbconfig);
    if (x11_untrap_errors() != 0)
        return -1;
    return glx_pixmap;
}

static int gen_fbo_data(vdpau_driver_data_t *driver_data,
                        GLuint               texture,
                        unsigned int         texture_width,
                        unsigned int         texture_height,
                        GLuint              *pfbo,
                        GLuint              *pfbo_buffer,
                        GLuint              *pfbo_texture)
{
    opengl_data_t * const gl_data = &driver_data->gl_data;
    GLuint fbo, fbo_buffer, fbo_texture;
    GLenum status;

    glGenTextures(1, &fbo_texture);
    glBindTexture(GL_TEXTURE_2D, fbo_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texture_width, texture_height, 0,
                 GL_BGRA, GL_UNSIGNED_BYTE, NULL);

    gl_data->gl_gen_framebuffers(1, &fbo);
    gl_data->gl_bind_framebuffer(GL_FRAMEBUFFER_EXT, fbo);
    gl_data->gl_gen_renderbuffers(1, &fbo_buffer);
    gl_data->gl_bind_renderbuffer(GL_RENDERBUFFER_EXT, fbo_buffer);

    glBindTexture(GL_TEXTURE_2D, texture);
    gl_data->gl_framebuffer_texture_2d(GL_FRAMEBUFFER_EXT,
                                       GL_COLOR_ATTACHMENT0_EXT,
                                       GL_TEXTURE_2D, texture, 0);

    status = gl_data->gl_check_framebuffer_status(GL_DRAW_FRAMEBUFFER_EXT);
    gl_data->gl_bind_framebuffer(GL_FRAMEBUFFER_EXT, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE_EXT)
        return -1;

    *pfbo         = fbo;
    *pfbo_buffer  = fbo_buffer;
    *pfbo_texture = fbo_texture;
    return 0;
}
#endif


/* ====================================================================== */
/* === VDPAU Gate                                                     === */
/* ====================================================================== */

// VdpVideoSurfaceCreate
static inline VdpStatus
vdpau_video_surface_create(vdpau_driver_data_t *driver_data,
                           VdpDevice            device,
                           VdpChromaType        chroma_type,
                           uint32_t             width,
                           uint32_t             height,
                           VdpVideoSurface     *surface)
{
    if (driver_data == NULL)
        return VDP_STATUS_INVALID_POINTER;
    if (driver_data->vdp_vtable.vdp_video_surface_create == NULL)
        return VDP_STATUS_INVALID_POINTER;
    return driver_data->vdp_vtable.vdp_video_surface_create(device,
                                                            chroma_type,
                                                            width,
                                                            height,
                                                            surface);
}

// VdpVideoSurfaceDestroy
static inline VdpStatus
vdpau_video_surface_destroy(vdpau_driver_data_t *driver_data,
                            VdpVideoSurface      surface)
{
    if (driver_data == NULL)
        return VDP_STATUS_INVALID_POINTER;
    if (driver_data->vdp_vtable.vdp_video_surface_destroy == NULL)
        return VDP_STATUS_INVALID_POINTER;
    return driver_data->vdp_vtable.vdp_video_surface_destroy(surface);
}

// VdpVideoSurfaceGetBitsYCbCr
static inline VdpStatus
vdpau_video_surface_get_bits_ycbcr(vdpau_driver_data_t *driver_data,
                                   VdpVideoSurface      surface,
                                   VdpYCbCrFormat       format,
                                   uint8_t            **dest,
                                   uint32_t            *stride)
{
    if (driver_data < 0)
        return VDP_STATUS_INVALID_POINTER;
    if (driver_data->vdp_vtable.vdp_video_surface_get_bits_ycbcr == NULL)
        return VDP_STATUS_INVALID_POINTER;
    return driver_data->vdp_vtable.vdp_video_surface_get_bits_ycbcr(surface, format,
                                                                    (void * const *)dest,
                                                                    stride);
}

// VdpOutputSurfaceCreate
static inline VdpStatus
vdpau_output_surface_create(vdpau_driver_data_t *driver_data,
                            VdpDevice            device,
                            VdpRGBAFormat        rgba_format,
                            uint32_t             width,
                            uint32_t             height,
                            VdpOutputSurface    *surface)
{
    if (driver_data == NULL)
        return VDP_STATUS_INVALID_POINTER;
    if (driver_data->vdp_vtable.vdp_output_surface_create == NULL)
        return VDP_STATUS_INVALID_POINTER;
    return driver_data->vdp_vtable.vdp_output_surface_create(device,
                                                             rgba_format,
                                                             width,
                                                             height,
                                                             surface);
}

// VdpOutputSurfaceDestroy
static inline VdpStatus
vdpau_output_surface_destroy(vdpau_driver_data_t *driver_data,
                             VdpOutputSurface     surface)
{
    if (driver_data == NULL)
        return VDP_STATUS_INVALID_POINTER;
    if (driver_data->vdp_vtable.vdp_output_surface_destroy == NULL)
        return VDP_STATUS_INVALID_POINTER;
    return driver_data->vdp_vtable.vdp_output_surface_destroy(surface);
}

// VdpOutputSurfaceGetBitsNative
static inline VdpStatus
vdpau_output_surface_get_bits_native(vdpau_driver_data_t *driver_data,
                                     VdpOutputSurface     surface,
                                     const VdpRect       *source_rect,
                                     uint8_t            **dst,
                                     uint32_t            *stride)
{
    if (driver_data == NULL)
        return VDP_STATUS_INVALID_POINTER;
    if (driver_data->vdp_vtable.vdp_output_surface_get_bits_native == NULL)
        return VDP_STATUS_INVALID_POINTER;
    return driver_data->vdp_vtable.vdp_output_surface_get_bits_native(surface,
                                                                      source_rect,
                                                                      (void * const *)dst,
                                                                      stride);
}

// VdpVideoMixerCreate
static inline VdpStatus
vdpau_video_mixer_create(vdpau_driver_data_t          *driver_data,
                         VdpDevice                     device,
                         uint32_t                      feature_count,
                         VdpVideoMixerFeature const   *features,
                         uint32_t                      parameter_count,
                         VdpVideoMixerParameter const *parameters,
                         const void                   *parameter_values,
                         VdpVideoMixer                *mixer)
{
    if (driver_data == NULL)
        return VDP_STATUS_INVALID_POINTER;
    if (driver_data->vdp_vtable.vdp_video_mixer_create == NULL)
        return VDP_STATUS_INVALID_POINTER;
    return driver_data->vdp_vtable.vdp_video_mixer_create(device,
                                                          feature_count,
                                                          features,
                                                          parameter_count,
                                                          parameters,
                                                          parameter_values,
                                                          mixer);
}

// VdpVideoMixerDestroy
static inline VdpStatus
vdpau_video_mixer_destroy(vdpau_driver_data_t *driver_data,
                          VdpVideoMixer        mixer)
{
    if (driver_data == NULL)
        return VDP_STATUS_INVALID_POINTER;
    if (driver_data->vdp_vtable.vdp_video_mixer_destroy == NULL)
        return VDP_STATUS_INVALID_POINTER;
    return driver_data->vdp_vtable.vdp_video_mixer_destroy(mixer);
}

// VdpVideoMixerRender
static inline VdpStatus
vdpau_video_mixer_render(vdpau_driver_data_t          *driver_data,
                         VdpVideoMixer                 mixer,
                         VdpOutputSurface              background_surface,
                         const VdpRect                *background_source_rect,
                         VdpVideoMixerPictureStructure current_picture_structure,
                         uint32_t                      video_surface_past_count,
                         const VdpVideoSurface        *video_surface_past,
                         VdpVideoSurface               video_surface_current,
                         uint32_t                      video_surface_future_count,
                         const VdpVideoSurface        *video_surface_future,
                         const VdpRect                *video_source_rect,
                         VdpOutputSurface              destination_surface,
                         const VdpRect                *destination_rect,
                         const VdpRect                *destination_video_rect,
                         uint32_t                      layer_count,
                         const VdpLayer               *layers)
{
    if (driver_data == NULL)
        return VDP_STATUS_INVALID_POINTER;
    if (driver_data->vdp_vtable.vdp_video_mixer_render == NULL)
        return VDP_STATUS_INVALID_POINTER;
    return driver_data->vdp_vtable.vdp_video_mixer_render(mixer,
                                                          background_surface,
                                                          background_source_rect,
                                                          current_picture_structure,
                                                          video_surface_past_count,
                                                          video_surface_past,
                                                          video_surface_current,
                                                          video_surface_future_count,
                                                          video_surface_future,
                                                          video_source_rect,
                                                          destination_surface,
                                                          destination_rect,
                                                          destination_video_rect,
                                                          layer_count,
                                                          layers);
}

// VdpPresentationQueueCreate
static inline VdpStatus
vdpau_presentation_queue_create(vdpau_driver_data_t       *driver_data,
                                VdpDevice                  device,
                                VdpPresentationQueueTarget presentation_queue_target,
                                VdpPresentationQueue      *presentation_queue)
{
    if (driver_data == NULL)
        return VDP_STATUS_INVALID_POINTER;
    if (driver_data->vdp_vtable.vdp_presentation_queue_create == NULL)
        return VDP_STATUS_INVALID_POINTER;
    return driver_data->vdp_vtable.vdp_presentation_queue_create(device,
                                                                 presentation_queue_target,
                                                                 presentation_queue);
}

// VdpPresentationQueueDestroy
static inline VdpStatus
vdpau_presentation_queue_destroy(vdpau_driver_data_t *driver_data,
                                 VdpPresentationQueue presentation_queue)
{
    if (driver_data == NULL)
        return VDP_STATUS_INVALID_POINTER;
    if (driver_data->vdp_vtable.vdp_presentation_queue_destroy == NULL)
        return VDP_STATUS_INVALID_POINTER;
    return driver_data->vdp_vtable.vdp_presentation_queue_destroy(presentation_queue);
}

// VdpPresentationQueueDisplay
static inline VdpStatus
vdpau_presentation_queue_display(vdpau_driver_data_t *driver_data,
                                 VdpPresentationQueue presentation_queue,
                                 VdpOutputSurface     surface,
                                 uint32_t             clip_width,
                                 uint32_t             clip_height,
                                 VdpTime              earliest_presentation_time)
{
    if (driver_data == NULL)
        return VDP_STATUS_INVALID_POINTER;
    if (driver_data->vdp_vtable.vdp_presentation_queue_display == NULL)
        return VDP_STATUS_INVALID_POINTER;
    return driver_data->vdp_vtable.vdp_presentation_queue_display(presentation_queue,
                                                                  surface,
                                                                  clip_width,
                                                                  clip_height,
                                                                  earliest_presentation_time);
}

// VdpPresentationQueueBlockUntilSurfaceIdle
static inline VdpStatus
vdpau_presentation_queue_block_until_surface_idle(vdpau_driver_data_t *driver_data,
                                                  VdpPresentationQueue presentation_queue,
                                                  VdpOutputSurface     surface,
                                                  VdpTime             *first_presentation_time)
{
    if (driver_data == NULL)
        return VDP_STATUS_INVALID_POINTER;
    if (driver_data->vdp_vtable.vdp_presentation_queue_block_until_surface_idle == NULL)
        return VDP_STATUS_INVALID_POINTER;
    return driver_data->vdp_vtable.vdp_presentation_queue_block_until_surface_idle(presentation_queue,
                                                                                   surface,
                                                                                   first_presentation_time);
}

// VdpPresentationQueueQuerySurfaceStatus
static inline VdpStatus
vdpau_presentation_queue_query_surface_status(vdpau_driver_data_t        *driver_data,
                                              VdpPresentationQueue        presentation_queue,
                                              VdpOutputSurface            surface,
                                              VdpPresentationQueueStatus *status,
                                              VdpTime                    *first_presentation_time)
{
    if (driver_data == NULL)
        return VDP_STATUS_INVALID_POINTER;
    if (driver_data->vdp_vtable.vdp_presentation_queue_query_surface_status == NULL)
        return VDP_STATUS_INVALID_POINTER;
    return driver_data->vdp_vtable.vdp_presentation_queue_query_surface_status(presentation_queue,
                                                                               surface,
                                                                               status,
                                                                               first_presentation_time);
}

// VdpPresentationQueueTargetCreateX11
static inline VdpStatus
vdpau_presentation_queue_target_create_x11(vdpau_driver_data_t        *driver_data,
                                           VdpDevice                   device,
                                           Drawable                    drawable,
                                           VdpPresentationQueueTarget *target)
{
    if (driver_data == NULL)
        return VDP_STATUS_INVALID_POINTER;
    if (driver_data->vdp_vtable.vdp_presentation_queue_target_create_x11 == NULL)
        return VDP_STATUS_INVALID_POINTER;
    return driver_data->vdp_vtable.vdp_presentation_queue_target_create_x11(device,
                                                                            drawable,
                                                                            target);
}

// VdpPresentationQueueTargetDestroy
static inline VdpStatus
vdpau_presentation_queue_target_destroy(vdpau_driver_data_t       *driver_data,
                                        VdpPresentationQueueTarget presentation_queue_target)
{
    if (driver_data == NULL)
        return VDP_STATUS_INVALID_POINTER;
    if (driver_data->vdp_vtable.vdp_presentation_queue_target_destroy == NULL)
        return VDP_STATUS_INVALID_POINTER;
    return driver_data->vdp_vtable.vdp_presentation_queue_target_destroy(presentation_queue_target);
}

// VdpDecoderCreate
static inline VdpStatus
vdpau_decoder_create(vdpau_driver_data_t *driver_data,
                     VdpDevice            device,
                     VdpDecoderProfile    profile,
                     uint32_t             width,
                     uint32_t             height,
                     uint32_t             max_references,
                     VdpDecoder          *decoder)
{
    if (driver_data == NULL)
        return VDP_STATUS_INVALID_POINTER;
    if (driver_data->vdp_vtable.vdp_decoder_create == NULL)
        return VDP_STATUS_INVALID_POINTER;
    return driver_data->vdp_vtable.vdp_decoder_create(device,
                                                      profile,
                                                      width,
                                                      height,
                                                      max_references,
                                                      decoder);
}

// VdpDecoderDestroy
static inline VdpStatus
vdpau_decoder_destroy(vdpau_driver_data_t *driver_data,
                      VdpDecoder           decoder)
{
    if (driver_data == NULL)
        return VDP_STATUS_INVALID_POINTER;
    if (driver_data->vdp_vtable.vdp_decoder_destroy == NULL)
        return VDP_STATUS_INVALID_POINTER;
    return driver_data->vdp_vtable.vdp_decoder_destroy(decoder);
}

// VdpDecoderRender
static inline VdpStatus
vdpau_decoder_render(vdpau_driver_data_t      *driver_data,
                     VdpDecoder                decoder,
                     VdpVideoSurface           target,
                     VdpPictureInfo const     *picture_info,
                     uint32_t                  bitstream_buffers_count,
                     VdpBitstreamBuffer const *bitstream_buffers)
                     
{
    if (driver_data == NULL)
        return VDP_STATUS_INVALID_POINTER;
    if (driver_data->vdp_vtable.vdp_decoder_render == NULL)
        return VDP_STATUS_INVALID_POINTER;
    return driver_data->vdp_vtable.vdp_decoder_render(decoder,
                                                      target,
                                                      picture_info,
                                                      bitstream_buffers_count,
                                                      bitstream_buffers);
}

// VdpDecoderQueryCapabilities
static inline VdpStatus
vdpau_decoder_query_capabilities(vdpau_driver_data_t *driver_data,
                                 VdpDevice            device,
                                 VdpDecoderProfile    profile,
                                 VdpBool             *is_supported,
                                 uint32_t            *max_level,
                                 uint32_t            *max_references,
                                 uint32_t            *max_width,
                                 uint32_t            *max_height)
{
    if (driver_data == NULL)
        return VDP_STATUS_INVALID_POINTER;
    if (driver_data->vdp_vtable.vdp_decoder_query_capabilities == NULL)
        return VDP_STATUS_INVALID_POINTER;
    return driver_data->vdp_vtable.vdp_decoder_query_capabilities(device,
                                                                  profile,
                                                                  is_supported,
                                                                  max_level,
                                                                  max_references,
                                                                  max_width,
                                                                  max_height);
}

// VdpVideoSurfaceQueryGetPutBitsYCbCrCapabilities
static inline VdpStatus
vdpau_video_surface_query_ycbcr_caps(vdpau_driver_data_t *driver_data,
                                     VdpDevice            device,
                                     VdpChromaType        surface_chroma_type,
                                     VdpYCbCrFormat       bits_ycbcr_format,
                                     VdpBool             *is_supported)
{
    if (driver_data == NULL)
        return VDP_STATUS_INVALID_POINTER;
    if (driver_data->vdp_vtable.vdp_video_surface_query_ycbcr_caps == NULL)
        return VDP_STATUS_INVALID_POINTER;
    return driver_data->vdp_vtable.vdp_video_surface_query_ycbcr_caps(device,
                                                                      surface_chroma_type,
                                                                      bits_ycbcr_format,
                                                                      is_supported);
}

// VdpOutputSurfaceQueryGetPutBitsNativeCapabilities
static inline VdpStatus
vdpau_output_surface_query_rgba_caps(vdpau_driver_data_t *driver_data,
                                     VdpDevice            device,
                                     VdpRGBAFormat        surface_rgba_format,
                                     VdpBool             *is_supported)
{
    if (driver_data == NULL)
        return VDP_STATUS_INVALID_POINTER;
    if (driver_data->vdp_vtable.vdp_output_surface_query_rgba_caps == NULL)
        return VDP_STATUS_INVALID_POINTER;
    return driver_data->vdp_vtable.vdp_output_surface_query_rgba_caps(device,
                                                                      surface_rgba_format,
                                                                      is_supported);
}

// VdpGetApiVersion
static inline VdpStatus
vdpau_get_api_version(vdpau_driver_data_t *driver_data, uint32_t *api_version)
{
    if (driver_data == NULL)
        return VDP_STATUS_INVALID_POINTER;
    if (driver_data->vdp_vtable.vdp_get_api_version == NULL)
        return VDP_STATUS_INVALID_POINTER;
    return driver_data->vdp_vtable.vdp_get_api_version(api_version);
}

// VdpGetInformationString
static inline VdpStatus
vdpau_get_information_string(vdpau_driver_data_t *driver_data,
                             const char         **info_string)
{
    if (driver_data == NULL)
        return VDP_STATUS_INVALID_POINTER;
    if (driver_data->vdp_vtable.vdp_get_information_string == NULL)
        return VDP_STATUS_INVALID_POINTER;
    return driver_data->vdp_vtable.vdp_get_information_string(info_string);
}

// VdpGetErrorString
static inline const char *
vdpau_get_error_string(vdpau_driver_data_t *driver_data, VdpStatus vdp_status)
{
    if (driver_data == NULL)
        return NULL;
    if (driver_data->vdp_vtable.vdp_get_error_string == NULL)
        return NULL;
    return driver_data->vdp_vtable.vdp_get_error_string(vdp_status);
}

// Checks whether the VDPAU implementation supports the specified profile
static inline VdpBool
vdpau_is_supported_profile(vdpau_driver_data_t *driver_data,
                           VdpDecoderProfile    profile)
{
    VdpBool is_supported = VDP_FALSE;
    VdpStatus vdp_status;
    uint32_t max_level, max_references, max_width, max_height;
    vdp_status = vdpau_decoder_query_capabilities(driver_data,
                                                  driver_data->vdp_device,
                                                  profile,
                                                  &is_supported,
                                                  &max_level,
                                                  &max_references,
                                                  &max_width,
                                                  &max_height);
    return vdp_status == VDP_STATUS_OK && is_supported;
}

// Checks whether the VDPAU implementation supports the specified image format
static inline VdpBool
vdpau_is_supported_image_format(vdpau_driver_data_t *driver_data,
                                VdpImageFormatType   type,
                                uint32_t             format)
{
    VdpBool is_supported = VDP_FALSE;
    VdpStatus vdp_status = VDP_STATUS_INVALID_VALUE;
    switch (type) {
    case VDP_IMAGE_FORMAT_TYPE_YCBCR:
        vdp_status = vdpau_video_surface_query_ycbcr_caps(driver_data,
                                                          driver_data->vdp_device,
                                                          VDP_CHROMA_TYPE_420,
                                                          format,
                                                          &is_supported);
        break;
    case VDP_IMAGE_FORMAT_TYPE_RGBA:
        vdp_status = vdpau_output_surface_query_rgba_caps(driver_data,
                                                          driver_data->vdp_device,
                                                          format,
                                                          &is_supported);
        break;
    }
    return vdp_status == VDP_STATUS_OK && is_supported;
}

// Returns the maximum dimensions supported by the VDPAU implementation for that profile
static inline VdpBool
vdpau_get_surface_size_max(vdpau_driver_data_t *driver_data,
                           VdpDecoderProfile    profile,
                           uint32_t            *pmax_width,
                           uint32_t            *pmax_height)
{
    VdpBool is_supported = VDP_FALSE;
    VdpStatus vdp_status;
    uint32_t max_level, max_references, max_width, max_height;
    vdp_status = vdpau_decoder_query_capabilities(driver_data,
                                                  driver_data->vdp_device,
                                                  profile,
                                                  &is_supported,
                                                  &max_level,
                                                  &max_references,
                                                  &max_width,
                                                  &max_height);

    if (pmax_width)
        *pmax_width = 0;
    if (pmax_height)
        *pmax_height = 0;

    if (vdp_status != VDP_STATUS_OK || !is_supported)
        return VDP_FALSE;

    if (pmax_width)
        *pmax_width = max_width;
    if (max_height)
        *pmax_height = max_height;

    return VDP_TRUE;
}

static inline VdpBool
vdpau_is_nvidia(vdpau_driver_data_t *driver_data, int *major, int *minor)
{
    uint32_t nvidia_version = 0;
    if (driver_data->vdp_impl_type == VDP_IMPLEMENTATION_NVIDIA)
        nvidia_version = driver_data->vdp_impl_version;
    if (major)
        *major = nvidia_version >> 16;
    if (minor)
        *minor = nvidia_version & 0xffff;
    return nvidia_version != 0;
}


/* ====================================================================== */
/* === VDPAU data dumpers                                             === */
/* ====================================================================== */

#define TRACE                   trace_print
#define INDENT(INC)             trace_indent(INC)

static int g_trace_enabled      = -1;
static int g_trace_is_new_line  = 1;
static int g_trace_indent       = 0;

static int check_vdpau_video_trace_env(void)
{
    const char *vdpau_video_trace_str = getenv("VDPAU_VIDEO_TRACE");
    /* XXX: check actual value */
    return vdpau_video_trace_str != NULL;
}

static inline int trace_enabled(void)
{
    if (g_trace_enabled < 0)
        g_trace_enabled = check_vdpau_video_trace_env();
    return g_trace_enabled;
}

static void trace_print(const char *format, ...)
{
    va_list args;

    if (g_trace_is_new_line) {
        int i;
        printf("%s: ", PACKAGE_NAME);
        for (i = 0; i < g_trace_indent; i++)
            printf("  ");
    }

    va_start(args, format);
    vfprintf(stdout, format, args);
    va_end(args);

    g_trace_is_new_line = (strchr(format, '\n') != NULL);

    if (g_trace_is_new_line)
        fflush(stdout);
}

static void trace_indent(int inc)
{
    g_trace_indent += inc;
}
#define DUMPx(S, M)             TRACE("." #M " = 0x%08x;\n", S->M)
#define DUMPi(S, M)             TRACE("." #M " = %d;\n", S->M)
#define DUMPm(S, M, I, J)       dump_matrix_NxM(#M, (uint8_t *)S->M, I, J, I * J)

// Dumps matrix[N][M] = N rows x M columns (uint8_t)
static void dump_matrix_NxM(const char *label, const uint8_t *matrix, int N, int M, int L)
{
    int i, j, n = 0;

    TRACE(".%s = {\n", label);
    INDENT(1);
    for (j = 0; j < N; j++) {
        for (i = 0; i < M; i++, n++) {
            if (n >= L)
                break;
            if (i > 0)
                TRACE(", ");
            TRACE("0x%02x", matrix[n]);
        }
        if (j < (N - 1))
            TRACE(",");
        TRACE("\n");
        if (n >= L)
            break;
    }
    INDENT(-1);
    TRACE("}\n");
}

static void dump_VdpPictureInfoMPEG1Or2(VdpPictureInfoMPEG1Or2 *pic_info)
{
    INDENT(1);
    TRACE("VdpPictureInfoMPEG1Or2 = {\n");
    INDENT(1);
    DUMPx(pic_info, forward_reference);
    DUMPx(pic_info, backward_reference);
    DUMPi(pic_info, slice_count);
    DUMPi(pic_info, picture_structure);
    DUMPi(pic_info, picture_coding_type);
    DUMPi(pic_info, intra_dc_precision);
    DUMPi(pic_info, frame_pred_frame_dct);
    DUMPi(pic_info, concealment_motion_vectors);
    DUMPi(pic_info, intra_vlc_format);
    DUMPi(pic_info, alternate_scan);
    DUMPi(pic_info, q_scale_type);
    DUMPi(pic_info, top_field_first);
    DUMPi(pic_info, full_pel_forward_vector);
    DUMPi(pic_info, full_pel_backward_vector);
    TRACE(".f_code = { { %d, %d }, { %d, %d } };\n",
          pic_info->f_code[0][0], pic_info->f_code[0][1],
          pic_info->f_code[1][0], pic_info->f_code[1][1]);
    DUMPm(pic_info, intra_quantizer_matrix, 8, 8);
    DUMPm(pic_info, non_intra_quantizer_matrix, 8, 8);
    INDENT(-1);
    TRACE("};\n");
    INDENT(-1);
}

static void dump_VdpReferenceFrameH264(const char *label, VdpReferenceFrameH264 *rf)
{
    TRACE(".%s = {\n", label);
    INDENT(1);
    DUMPx(rf, surface);
    DUMPi(rf, is_long_term);
    DUMPi(rf, top_is_reference);
    DUMPi(rf, bottom_is_reference);
    DUMPi(rf, field_order_cnt[0]);
    DUMPi(rf, field_order_cnt[1]);
    DUMPi(rf, frame_idx);
    INDENT(-1);
    TRACE("}\n");
}

static void dump_VdpPictureInfoH264(VdpPictureInfoH264 *pic_info)
{
    int i;

    INDENT(1);
    TRACE("VdpPictureInfoH264 = {\n");
    INDENT(1);
    DUMPi(pic_info, slice_count);
    DUMPi(pic_info, field_order_cnt[0]);
    DUMPi(pic_info, field_order_cnt[1]);
    DUMPi(pic_info, is_reference);
    DUMPi(pic_info, frame_num);
    DUMPi(pic_info, field_pic_flag);
    DUMPi(pic_info, bottom_field_flag);
    DUMPi(pic_info, num_ref_frames);
    DUMPi(pic_info, mb_adaptive_frame_field_flag);
    DUMPi(pic_info, constrained_intra_pred_flag);
    DUMPi(pic_info, weighted_pred_flag);
    DUMPi(pic_info, weighted_bipred_idc);
    DUMPi(pic_info, frame_mbs_only_flag);
    DUMPi(pic_info, transform_8x8_mode_flag);
    DUMPi(pic_info, chroma_qp_index_offset);
    DUMPi(pic_info, second_chroma_qp_index_offset);
    DUMPi(pic_info, pic_init_qp_minus26);
    DUMPi(pic_info, num_ref_idx_l0_active_minus1);
    DUMPi(pic_info, num_ref_idx_l1_active_minus1);
    DUMPi(pic_info, log2_max_frame_num_minus4);
    DUMPi(pic_info, pic_order_cnt_type);
    DUMPi(pic_info, log2_max_pic_order_cnt_lsb_minus4);
    DUMPi(pic_info, delta_pic_order_always_zero_flag);
    DUMPi(pic_info, direct_8x8_inference_flag);
    DUMPi(pic_info, entropy_coding_mode_flag);
    DUMPi(pic_info, pic_order_present_flag);
    DUMPi(pic_info, deblocking_filter_control_present_flag);
    DUMPi(pic_info, redundant_pic_cnt_present_flag);
    DUMPm(pic_info, scaling_lists_4x4, 6, 16);
    DUMPm(pic_info, scaling_lists_8x8[0], 8, 8);
    DUMPm(pic_info, scaling_lists_8x8[1], 8, 8);
    for (i = 0; i < 16; i++) {
        char label[100];
        sprintf(label, "referenceFrames[%d]", i);
        dump_VdpReferenceFrameH264(label, &pic_info->referenceFrames[i]);
    }
    INDENT(-1);
    TRACE("};\n");
    INDENT(-1);
}


static void dump_VdpPictureInfoVC1(VdpPictureInfoVC1 *pic_info)
{
    INDENT(1);
    TRACE("VdpPictureInfoVC1 = {\n");
    INDENT(1);
    DUMPx(pic_info, forward_reference);
    DUMPx(pic_info, backward_reference);
    DUMPi(pic_info, slice_count);
    DUMPi(pic_info, picture_type);
    DUMPi(pic_info, frame_coding_mode);
    DUMPi(pic_info, postprocflag);
    DUMPi(pic_info, pulldown);
    DUMPi(pic_info, interlace);
    DUMPi(pic_info, tfcntrflag);
    DUMPi(pic_info, finterpflag);
    DUMPi(pic_info, psf);
    DUMPi(pic_info, dquant);
    DUMPi(pic_info, panscan_flag);
    DUMPi(pic_info, refdist_flag);
    DUMPi(pic_info, quantizer);
    DUMPi(pic_info, extended_mv);
    DUMPi(pic_info, extended_dmv);
    DUMPi(pic_info, overlap);
    DUMPi(pic_info, vstransform);
    DUMPi(pic_info, loopfilter);
    DUMPi(pic_info, fastuvmc);
    DUMPi(pic_info, range_mapy_flag);
    DUMPi(pic_info, range_mapy);
    DUMPi(pic_info, range_mapuv_flag);
    DUMPi(pic_info, range_mapuv);
    DUMPi(pic_info, multires);
    DUMPi(pic_info, syncmarker);
    DUMPi(pic_info, rangered);
    DUMPi(pic_info, maxbframes);
    DUMPi(pic_info, deblockEnable);
    DUMPi(pic_info, pquant);
    INDENT(-1);
    TRACE("};\n");
    INDENT(-1);
}

static void dump_VdpBitstreamBuffer(VdpBitstreamBuffer *bitstream_buffer)
{
    const uint8_t *buffer = bitstream_buffer->bitstream;
    const uint32_t size   = bitstream_buffer->bitstream_bytes;

    INDENT(1);
    TRACE("VdpBitstreamBuffer (%d bytes) = {\n", size);
    INDENT(1);
    dump_matrix_NxM("buffer", buffer, 10, 15, size);
    INDENT(-1);
    TRACE("};\n");
    INDENT(-1);
}


/* ====================================================================== */
/* === VA API to VDPAU thunks                                         === */
/* ====================================================================== */

static VdpBitstreamBuffer *
vdpau_allocate_VdpBitstreamBuffer(object_context_p obj_context)
{
    VdpBitstreamBuffer *vdp_bitstream_buffers = obj_context->vdp_bitstream_buffers;
    if (obj_context->vdp_bitstream_buffers_count + 1 > obj_context->vdp_bitstream_buffers_count_max) {
        obj_context->vdp_bitstream_buffers_count_max += 16;
        vdp_bitstream_buffers = realloc(vdp_bitstream_buffers,
                                        obj_context->vdp_bitstream_buffers_count_max * sizeof(vdp_bitstream_buffers[0]));
        if (!vdp_bitstream_buffers)
            return NULL;
        obj_context->vdp_bitstream_buffers = vdp_bitstream_buffers;
    }
    return &vdp_bitstream_buffers[obj_context->vdp_bitstream_buffers_count++];
}

static int
vdpau_append_VdpBitstreamBuffer(object_context_p obj_context,
                                const uint8_t   *buffer,
                                uint32_t         buffer_size)
{
    VdpBitstreamBuffer *bitstream_buffer;

    if ((bitstream_buffer = vdpau_allocate_VdpBitstreamBuffer(obj_context)) == NULL)
        return 0;

    bitstream_buffer->struct_version    = VDP_BITSTREAM_BUFFER_VERSION;
    bitstream_buffer->bitstream         = buffer;
    bitstream_buffer->bitstream_bytes   = buffer_size;
    return 1;
}

static VAStatus vdpau_translate_VAStatus(vdpau_driver_data_t *driver_data,
                                         VdpStatus            vdp_status)
{
    VAStatus va_status;
    const char *vdp_status_string;

    switch (vdp_status) {
    case VDP_STATUS_OK:
        va_status = VA_STATUS_SUCCESS;
        break;
    case VDP_STATUS_INVALID_DECODER_PROFILE:
        va_status = VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
        break;
    default:
        vdp_status_string = vdpau_get_error_string(driver_data, vdp_status);
        D(bug("WARNING: unknown VdpStatus %d: %s\n", vdp_status,
              vdp_status_string ? vdp_status_string : "<unknown error>"));
        va_status = VA_STATUS_ERROR_UNKNOWN;
        break;
    }
    return va_status;
}

static int
vdpau_translate_VASurfaceID(vdpau_driver_data_t *driver_data,
                            VASurfaceID          va_surface,
                            VdpVideoSurface     *vdp_surface)
{
    if (va_surface == VA_INVALID_SURFACE) {
        *vdp_surface = VDP_INVALID_HANDLE;
        return 1;
    }
    object_surface_p obj_surface = SURFACE(va_surface);
    ASSERT(obj_surface);
    if (obj_surface == NULL)
        return 0;
    *vdp_surface = obj_surface->vdp_surface;
    return 1;
}

static int
vdpau_translate_nothing(vdpau_driver_data_t *driver_data,
                        object_context_p     obj_context,
                        object_buffer_p      obj_buffer)
{
    return 1;
}

static void
vdpau_h264_clear_reference_frames(object_context_p obj_context);

static int
vdpau_translate_VASliceDataBuffer(vdpau_driver_data_t *driver_data,
                                  object_context_p     obj_context,
                                  object_buffer_p      obj_buffer)
{
    if (obj_context->vdp_codec == VDP_CODEC_H264) {
        /* Check we have the start code */
        /* XXX: check for other codecs too? */
        /* XXX: this assumes we get SliceParams before SliceData */
        static const uint8_t start_code_prefix[3] = { 0x00, 0x00, 0x01 };
        VASliceParameterBufferH264 * const slice_params = obj_context->last_slice_params;
        unsigned int i;
        for (i = 0; i < obj_context->last_slice_params_count; i++) {
            VASliceParameterBufferH264 * const slice_param = &slice_params[i];
            uint8_t *buf = (uint8_t *)obj_buffer->buffer_data + slice_param->slice_data_offset;
            if (memcmp(buf, start_code_prefix, sizeof(start_code_prefix)) != 0) {
                if (!vdpau_append_VdpBitstreamBuffer(obj_context, start_code_prefix, sizeof(start_code_prefix)))
                    return 0;
            }
            if ((buf[0] & 0x1f) == 5) /* IDR */
                vdpau_h264_clear_reference_frames(obj_context);
            if (!vdpau_append_VdpBitstreamBuffer(obj_context, buf, slice_param->slice_data_size))
                return 0;
        }
        return 1;
    }

    return vdpau_append_VdpBitstreamBuffer(obj_context,
                                           obj_buffer->buffer_data,
                                           obj_buffer->buffer_size);
}

static int
vdpau_translate_VAPictureParameterBufferMPEG2(vdpau_driver_data_t *driver_data,
                                              object_context_p     obj_context,
                                              object_buffer_p      obj_buffer)
{
    VdpPictureInfoMPEG1Or2 * const pinfo = &obj_context->vdp_picture_info.mpeg2;
    VAPictureParameterBufferMPEG2 * const pic_param = obj_buffer->buffer_data;
    if (!vdpau_translate_VASurfaceID(driver_data,
                                     pic_param->forward_reference_picture,
                                     &pinfo->forward_reference))
        return 0;
    if (!vdpau_translate_VASurfaceID(driver_data,
                                     pic_param->backward_reference_picture,
                                     &pinfo->backward_reference))
        return 0;
    pinfo->picture_structure            = pic_param->picture_coding_extension.bits.picture_structure;
    pinfo->picture_coding_type          = pic_param->picture_coding_type;
    pinfo->intra_dc_precision           = pic_param->picture_coding_extension.bits.intra_dc_precision;
    pinfo->frame_pred_frame_dct         = pic_param->picture_coding_extension.bits.frame_pred_frame_dct;
    pinfo->concealment_motion_vectors   = pic_param->picture_coding_extension.bits.concealment_motion_vectors;
    pinfo->intra_vlc_format             = pic_param->picture_coding_extension.bits.intra_vlc_format;
    pinfo->alternate_scan               = pic_param->picture_coding_extension.bits.alternate_scan;
    pinfo->q_scale_type                 = pic_param->picture_coding_extension.bits.q_scale_type;
    pinfo->top_field_first              = pic_param->picture_coding_extension.bits.top_field_first;
    pinfo->full_pel_forward_vector      = 0;
    pinfo->full_pel_backward_vector     = 0;
    pinfo->f_code[0][0]                 = (pic_param->f_code >> 12) & 0xf;
    pinfo->f_code[0][1]                 = (pic_param->f_code >>  8) & 0xf;
    pinfo->f_code[1][0]                 = (pic_param->f_code >>  4) & 0xf;
    pinfo->f_code[1][1]                 = pic_param->f_code & 0xf;
    return 1;
}

static const uint8_t ff_identity[64] = {
    0,   1,  2,  3,  4,  5,  6,  7,
    8,   9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23,
    24, 25, 26, 27, 28, 29, 30, 31,
    32, 33, 34, 35, 36, 37, 38, 39,
    40, 41, 42, 43, 44, 45, 46, 47,
    48, 49, 50, 51, 52, 53, 54, 55,
    56, 57, 58, 59, 60, 61, 62, 63
};

static const uint8_t ff_zigzag_direct[64] = {
    0,   1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

static const uint8_t ff_mpeg1_default_intra_matrix[64] = {
     8, 16, 19, 22, 26, 27, 29, 34,
    16, 16, 22, 24, 27, 29, 34, 37,
    19, 22, 26, 27, 29, 34, 34, 38,
    22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48,
    26, 27, 29, 32, 35, 40, 48, 58,
    26, 27, 29, 34, 38, 46, 56, 69,
    27, 29, 35, 38, 46, 56, 69, 83
};

static const uint8_t ff_mpeg1_default_non_intra_matrix[64] = {
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16
};

static int
vdpau_translate_VAIQMatrixBufferMPEG2(vdpau_driver_data_t *driver_data,
                                      object_context_p     obj_context,
                                      object_buffer_p      obj_buffer)
{
    VdpPictureInfoMPEG1Or2 * const pinfo = &obj_context->vdp_picture_info.mpeg2;
    VAIQMatrixBufferMPEG2 * const iq_matrix = obj_buffer->buffer_data;

    const uint8_t *intra_matrix;
    const uint8_t *intra_matrix_lookup;
    const uint8_t *inter_matrix;
    const uint8_t *inter_matrix_lookup;
    if (iq_matrix->load_intra_quantiser_matrix) {
        intra_matrix = iq_matrix->intra_quantiser_matrix;
        intra_matrix_lookup = ff_zigzag_direct;
    }
    else {
        intra_matrix = ff_mpeg1_default_intra_matrix;
        intra_matrix_lookup = ff_identity;
    }
    if (iq_matrix->load_non_intra_quantiser_matrix) {
        inter_matrix = iq_matrix->non_intra_quantiser_matrix;
        inter_matrix_lookup = ff_zigzag_direct;
    }
    else {
        inter_matrix = ff_mpeg1_default_non_intra_matrix;
        inter_matrix_lookup = ff_identity;
    }

    int i;
    for (i = 0; i < 64; i++) {
        pinfo->intra_quantizer_matrix[intra_matrix_lookup[i]] =
            intra_matrix[i];
        pinfo->non_intra_quantizer_matrix[inter_matrix_lookup[i]] =
            inter_matrix[i];
    }
    return 1;
}

static int
vdpau_translate_VASliceParameterBufferMPEG2(vdpau_driver_data_t *driver_data,
                                            object_context_p     obj_context,
                                            object_buffer_p      obj_buffer)
{
    VdpPictureInfoMPEG1Or2 * const pinfo = &obj_context->vdp_picture_info.mpeg2;

    pinfo->slice_count                 += obj_buffer->num_elements;
    obj_context->last_slice_params      = obj_buffer->buffer_data;
    obj_context->last_slice_params_count= obj_buffer->num_elements;
    return 1;
}

static void
vdpau_init_VdpReferenceFrameH264(VdpReferenceFrameH264 *rf)
{
    rf->surface                 = VDP_INVALID_HANDLE;
    rf->is_long_term            = VDP_FALSE;
    rf->top_is_reference        = VDP_FALSE;
    rf->bottom_is_reference     = VDP_FALSE;
    rf->field_order_cnt[0]      = 0;
    rf->field_order_cnt[1]      = 0;
    rf->frame_idx               = 0;
}

static int
vdpau_translate_VAPictureH264(vdpau_driver_data_t   *driver_data,
                              const VAPictureH264   *va_pic,
                              VdpReferenceFrameH264 *rf)
{
    // Handle invalid surfaces specifically
    if (va_pic->picture_id == VA_INVALID_SURFACE) {
        vdpau_init_VdpReferenceFrameH264(rf);
        return 1;
    }

    if (!vdpau_translate_VASurfaceID(driver_data, va_pic->picture_id, &rf->surface))
        return 0;
    rf->is_long_term            = (va_pic->flags & VA_PICTURE_H264_LONG_TERM_REFERENCE) != 0;
    if ((va_pic->flags & (VA_PICTURE_H264_TOP_FIELD|VA_PICTURE_H264_BOTTOM_FIELD)) == 0) {
        rf->top_is_reference    = VDP_TRUE;
        rf->bottom_is_reference = VDP_TRUE;
    }
    else {
        rf->top_is_reference    = (va_pic->flags & VA_PICTURE_H264_TOP_FIELD) != 0;
        rf->bottom_is_reference = (va_pic->flags & VA_PICTURE_H264_BOTTOM_FIELD) != 0;
    }
    rf->field_order_cnt[0]      = va_pic->TopFieldOrderCnt;
    rf->field_order_cnt[1]      = va_pic->BottomFieldOrderCnt;
    rf->frame_idx               = va_pic->frame_idx;
    return 1;
}

static int
vdpau_sync_VAPictureH264(vdpau_driver_data_t    *driver_data,
                         const VAPictureH264    *va_pic)
{
    if (va_pic->picture_id == VA_INVALID_SURFACE)
        return 1;
    object_surface_p obj_surface = SURFACE(va_pic->picture_id);
    ASSERT(obj_surface);
    if (obj_surface == NULL)
        return 0;
    return vdpau_translate_VAPictureH264(driver_data, va_pic, &obj_surface->vdp_ref_frame.h264);
}

static void
vdpau_h264_clear_reference_frames(object_context_p obj_context)
{
    unsigned int i;

    if (!vdpau_video_dpb())
        return;

    obj_context->ref_frames_count       = 0;
    for (i = 0; i < ARRAY_ELEMS(obj_context->ref_frames); i++)
        obj_context->ref_frames[i]      = VA_INVALID_SURFACE;
}

static int
vdpau_h264_sync_reference_frames(vdpau_driver_data_t            *driver_data,
                                 object_context_p                obj_context,
                                 VAPictureParameterBufferH264   *pic_param)
{
    VdpPictureInfoH264 * const pinfo = &obj_context->vdp_picture_info.h264;
    VAPictureH264 * const CurrPic = &pic_param->CurrPic;
    unsigned int i;

    /* Synchronize plain VAPictureParameterBufferH264.ReferenceFrames */
    if (!vdpau_video_dpb()) {
        for (i = 0; i < 16; i++) {
            if (!vdpau_translate_VAPictureH264(driver_data,
                                               &pic_param->ReferenceFrames[i],
                                               &pinfo->referenceFrames[i]))
                return 0;
        }
        return 1;
    }

    /* Synchronize past reference frames */
    for (i = 0; i < ARRAY_ELEMS(pic_param->ReferenceFrames); i++) {
        VAPictureH264 * const va_pic = &pic_param->ReferenceFrames[i];
        if (!vdpau_sync_VAPictureH264(driver_data, va_pic))
            return 0;
    }

    /* Remove current picture if it is not the second field of a
       reference field picture */
    for (i = 0; i < obj_context->ref_frames_count; i++) {
        if (obj_context->ref_frames[i] == CurrPic->picture_id) {
            if (!pinfo->field_pic_flag) {
                for (i = i + 1; i < obj_context->ref_frames_count; i++)
                    obj_context->ref_frames[i - 1] = obj_context->ref_frames[i];
                obj_context->ref_frames[--obj_context->ref_frames_count] = VA_INVALID_SURFACE;
            }
            break;
        }
    }

    /* Fill in VDPAU referenceFrames[] array */
    for (i = 0; i < obj_context->ref_frames_count; i++) {
        object_surface_p obj_surface = SURFACE(obj_context->ref_frames[i]);
        ASSERT(obj_surface);
        if (obj_surface == NULL)
            return 0;
        pinfo->referenceFrames[i] = obj_surface->vdp_ref_frame.h264;
    }
    for (; i < ARRAY_ELEMS(pinfo->referenceFrames); i++)
        vdpau_init_VdpReferenceFrameH264(&pinfo->referenceFrames[i]);

    /* Synchronize current picture, no matter it is reference or not */
    return vdpau_sync_VAPictureH264(driver_data, CurrPic);
}

static void
vdpau_h264_update_reference_frames(vdpau_driver_data_t  *driver_data,
                                   object_context_p      obj_context)
{
    VdpPictureInfoH264 * const pic_info = &obj_context->vdp_picture_info.h264;
    VASurfaceID picture_id = obj_context->current_render_target;
    unsigned int i;

    if (!vdpau_video_dpb())
        return;

    /* Remove non-reference picture that was previously a reference (in DPB) */
    if (!pic_info->is_reference) {
        for (i = 0; i < obj_context->ref_frames_count; i++) {
            if (obj_context->ref_frames[i] == picture_id) {
                for (i = i + 1; i < obj_context->ref_frames_count; i++)
                    obj_context->ref_frames[i - 1] = obj_context->ref_frames[i];
                obj_context->ref_frames[--obj_context->ref_frames_count] = VA_INVALID_SURFACE;
                break;
            }
        }
        return;
    }

    if (obj_context->max_ref_frames == 0)
        return;

    /* Update (return) if we already had this reference picture in DPB */
    for (i = 0; i < obj_context->ref_frames_count; i++) {
        if (obj_context->ref_frames[i] == picture_id)
            return;
    }

    /* Shift one frame buffer out of the DPB, and add the new reference picture */
    if (obj_context->ref_frames_count >= obj_context->max_ref_frames) {
        for (i = 1; i < obj_context->ref_frames_count; i++)
            obj_context->ref_frames[i - 1] = obj_context->ref_frames[i];
        obj_context->ref_frames[--obj_context->ref_frames_count] = VA_INVALID_SURFACE;
    }
    ASSERT(obj_context->ref_frames_count < obj_context->max_ref_frames);

    obj_context->ref_frames[obj_context->ref_frames_count] = picture_id;
    obj_context->ref_frames_count++;
}

static int
vdpau_translate_VAPictureParameterBufferH264(vdpau_driver_data_t *driver_data,
                                             object_context_p     obj_context,
                                             object_buffer_p      obj_buffer)
{
    VdpPictureInfoH264 * const pinfo = &obj_context->vdp_picture_info.h264;
    VAPictureParameterBufferH264 * const pic_param = obj_buffer->buffer_data;
    VAPictureH264 * const CurrPic = &pic_param->CurrPic;

    pinfo->field_order_cnt[0]                   = CurrPic->TopFieldOrderCnt;
    pinfo->field_order_cnt[1]                   = CurrPic->BottomFieldOrderCnt;
    pinfo->is_reference                         = pic_param->pic_fields.bits.reference_pic_flag;

    pinfo->frame_num                            = pic_param->frame_num;
    pinfo->field_pic_flag                       = pic_param->pic_fields.bits.field_pic_flag;
    pinfo->bottom_field_flag                    = pic_param->pic_fields.bits.field_pic_flag && (CurrPic->flags & VA_PICTURE_H264_BOTTOM_FIELD) != 0;
    pinfo->num_ref_frames                       = pic_param->num_ref_frames;
    pinfo->mb_adaptive_frame_field_flag         = pic_param->seq_fields.bits.mb_adaptive_frame_field_flag && !pinfo->field_pic_flag;
    pinfo->constrained_intra_pred_flag          = pic_param->pic_fields.bits.constrained_intra_pred_flag;
    pinfo->weighted_pred_flag                   = pic_param->pic_fields.bits.weighted_pred_flag;
    pinfo->weighted_bipred_idc                  = pic_param->pic_fields.bits.weighted_bipred_idc;
    pinfo->frame_mbs_only_flag                  = pic_param->seq_fields.bits.frame_mbs_only_flag;
    pinfo->transform_8x8_mode_flag              = pic_param->pic_fields.bits.transform_8x8_mode_flag;
    pinfo->chroma_qp_index_offset               = pic_param->chroma_qp_index_offset;
    pinfo->second_chroma_qp_index_offset        = pic_param->second_chroma_qp_index_offset;
    pinfo->pic_init_qp_minus26                  = pic_param->pic_init_qp_minus26;
    pinfo->log2_max_frame_num_minus4            = pic_param->seq_fields.bits.log2_max_frame_num_minus4;
    pinfo->pic_order_cnt_type                   = pic_param->seq_fields.bits.pic_order_cnt_type;
    pinfo->log2_max_pic_order_cnt_lsb_minus4    = pic_param->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4;
    pinfo->delta_pic_order_always_zero_flag     = pic_param->seq_fields.bits.delta_pic_order_always_zero_flag;
    pinfo->direct_8x8_inference_flag            = pic_param->seq_fields.bits.direct_8x8_inference_flag;
    pinfo->entropy_coding_mode_flag             = pic_param->pic_fields.bits.entropy_coding_mode_flag;
    pinfo->pic_order_present_flag               = pic_param->pic_fields.bits.pic_order_present_flag;
    pinfo->deblocking_filter_control_present_flag = pic_param->pic_fields.bits.deblocking_filter_control_present_flag;
    pinfo->redundant_pic_cnt_present_flag       = pic_param->pic_fields.bits.redundant_pic_cnt_present_flag;

    return vdpau_h264_sync_reference_frames(driver_data, obj_context, pic_param);
}

static int
vdpau_translate_VAIQMatrixBufferH264(vdpau_driver_data_t *driver_data,
                                      object_context_p     obj_context,
                                      object_buffer_p      obj_buffer)
{
    VdpPictureInfoH264 * const pinfo = &obj_context->vdp_picture_info.h264;
    VAIQMatrixBufferH264 * const iq_matrix = obj_buffer->buffer_data;
    int i, j;

    if (sizeof(pinfo->scaling_lists_4x4) == sizeof(iq_matrix->ScalingList4x4))
        memcpy(pinfo->scaling_lists_4x4, iq_matrix->ScalingList4x4,
               sizeof(pinfo->scaling_lists_4x4));
    else {
        for (j = 0; j < 6; j++) {
            for (i = 0; i < 16; i++)
                pinfo->scaling_lists_4x4[j][i] = iq_matrix->ScalingList4x4[j][i];
        }
    }

    if (sizeof(pinfo->scaling_lists_8x8) == sizeof(iq_matrix->ScalingList8x8))
        memcpy(pinfo->scaling_lists_8x8, iq_matrix->ScalingList8x8,
               sizeof(pinfo->scaling_lists_8x8));
    else {
        for (j = 0; j < 2; j++) {
            for (i = 0; i < 64; i++)
                pinfo->scaling_lists_8x8[j][i] = iq_matrix->ScalingList8x8[j][i];
        }
    }
    return 1;
}

static int
vdpau_translate_VASliceParameterBufferH264(vdpau_driver_data_t *driver_data,
                                            object_context_p     obj_context,
                                            object_buffer_p      obj_buffer)
{
    VdpPictureInfoH264 * const pinfo = &obj_context->vdp_picture_info.h264;
    VASliceParameterBufferH264 * const slice_params = obj_buffer->buffer_data;
    VASliceParameterBufferH264 * const slice_param = &slice_params[obj_buffer->num_elements - 1];

    pinfo->slice_count                 += obj_buffer->num_elements;
    pinfo->num_ref_idx_l0_active_minus1 = slice_param->num_ref_idx_l0_active_minus1;
    pinfo->num_ref_idx_l1_active_minus1 = slice_param->num_ref_idx_l1_active_minus1;
    obj_context->last_slice_params      = obj_buffer->buffer_data;
    obj_context->last_slice_params_count= obj_buffer->num_elements;
    return 1;
}

static int
vdpau_translate_VAPictureParameterBufferVC1(vdpau_driver_data_t *driver_data,
                                            object_context_p     obj_context,
                                            object_buffer_p      obj_buffer)
{
    VdpPictureInfoVC1 * const pinfo = &obj_context->vdp_picture_info.vc1;
    VAPictureParameterBufferVC1 * const pic_param = obj_buffer->buffer_data;
    int picture_type, major_version, minor_version;

    if (!vdpau_translate_VASurfaceID(driver_data,
                                     pic_param->forward_reference_picture,
                                     &pinfo->forward_reference))
        return 0;
    if (!vdpau_translate_VASurfaceID(driver_data,
                                     pic_param->backward_reference_picture,
                                     &pinfo->backward_reference))
        return 0;

    switch (pic_param->picture_fields.bits.picture_type) {
    case 0: picture_type = 0; break; /* I */
    case 1: picture_type = 1; break; /* P */
    case 2: picture_type = 3; break; /* B */
    case 3: picture_type = 4; break; /* BI */
    case 4: picture_type = 1; break; /* P "skipped" */
    default: ASSERT(!pic_param->picture_fields.bits.picture_type); return 0;
    }

    pinfo->picture_type         = picture_type;
    pinfo->frame_coding_mode    = pic_param->picture_fields.bits.frame_coding_mode;
    pinfo->postprocflag         = pic_param->post_processing != 0;
    pinfo->pulldown             = pic_param->sequence_fields.bits.pulldown;
    pinfo->interlace            = pic_param->sequence_fields.bits.interlace;
    pinfo->tfcntrflag           = pic_param->sequence_fields.bits.tfcntrflag;
    pinfo->finterpflag          = pic_param->sequence_fields.bits.finterpflag;
    pinfo->psf                  = pic_param->sequence_fields.bits.psf;
    pinfo->dquant               = pic_param->pic_quantizer_fields.bits.dquant;
    pinfo->panscan_flag         = pic_param->entrypoint_fields.bits.panscan_flag;
    pinfo->refdist_flag         = pic_param->reference_fields.bits.reference_distance_flag;
    pinfo->quantizer            = pic_param->pic_quantizer_fields.bits.quantizer;
    pinfo->extended_mv          = pic_param->mv_fields.bits.extended_mv_flag;
    pinfo->extended_dmv         = pic_param->mv_fields.bits.extended_dmv_flag;
    pinfo->overlap              = pic_param->sequence_fields.bits.overlap;
    pinfo->vstransform          = pic_param->transform_fields.bits.variable_sized_transform_flag;
    pinfo->loopfilter           = pic_param->entrypoint_fields.bits.loopfilter;
    pinfo->fastuvmc             = pic_param->fast_uvmc_flag;
    pinfo->range_mapy_flag      = pic_param->range_mapping_fields.bits.luma_flag;
    pinfo->range_mapy           = pic_param->range_mapping_fields.bits.luma;
    pinfo->range_mapuv_flag     = pic_param->range_mapping_fields.bits.chroma_flag;
    pinfo->range_mapuv          = pic_param->range_mapping_fields.bits.chroma;
    pinfo->multires             = pic_param->sequence_fields.bits.multires;
    pinfo->syncmarker           = pic_param->sequence_fields.bits.syncmarker;
    pinfo->rangered             = pic_param->sequence_fields.bits.rangered;
    if (!vdpau_is_nvidia(driver_data, &major_version, &minor_version) ||
        (major_version > 180 || minor_version >= 35))
        pinfo->rangered         |= pic_param->range_reduction_frame << 1;
    pinfo->maxbframes           = pic_param->sequence_fields.bits.max_b_frames;
    pinfo->deblockEnable        = pic_param->post_processing != 0; /* XXX: this is NVIDIA's vdpau.c semantics (postprocflag & 1) */
    pinfo->pquant               = pic_param->pic_quantizer_fields.bits.pic_quantizer_scale;
    return 1;
}

static int
vdpau_translate_VASliceParameterBufferVC1(vdpau_driver_data_t *driver_data,
                                          object_context_p     obj_context,
                                          object_buffer_p      obj_buffer)
{
    VdpPictureInfoVC1 * const pinfo = &obj_context->vdp_picture_info.vc1;

    pinfo->slice_count                 += obj_buffer->num_elements;
    obj_context->last_slice_params      = obj_buffer->buffer_data;
    obj_context->last_slice_params_count= obj_buffer->num_elements;
    return 1;
}

typedef int (*vdpau_translate_buffer_func_t)(vdpau_driver_data_t *driver_data,
                                             object_context_p     obj_context,
                                             object_buffer_p      obj_buffer);

typedef struct vdpau_translate_buffer_info vdpau_translate_buffer_info_t;
struct vdpau_translate_buffer_info {
    VdpCodec codec;
    VABufferType type;
    vdpau_translate_buffer_func_t func;
};

static int
vdpau_translate_buffer(vdpau_driver_data_t *driver_data,
                       object_context_p     obj_context,
                       object_buffer_p      obj_buffer)
{
    static const vdpau_translate_buffer_info_t translate_info[] = {
#define _(CODEC, TYPE)                                  \
        { VDP_CODEC_##CODEC, VA##TYPE##BufferType,      \
          vdpau_translate_VA##TYPE##Buffer##CODEC }
        _(MPEG2, PictureParameter),
        _(MPEG2, IQMatrix),
        _(MPEG2, SliceParameter),
        _(H264, PictureParameter),
        _(H264, IQMatrix),
        _(H264, SliceParameter),
        _(VC1, PictureParameter),
        _(VC1, SliceParameter),
#undef _
        { VDP_CODEC_VC1, VABitPlaneBufferType, vdpau_translate_nothing },
        { 0, VASliceDataBufferType, vdpau_translate_VASliceDataBuffer },
        { 0, 0, NULL }
    };
    const vdpau_translate_buffer_info_t *tbip;
    for (tbip = translate_info; tbip->func != NULL; tbip++) {
        if (tbip->codec && tbip->codec != obj_context->vdp_codec)
            continue;
        if (tbip->type != obj_buffer->type)
            continue;
        return tbip->func(driver_data, obj_context, obj_buffer);
    }
    D(bug("ERROR: no translate function found for %s%s\n",
          string_of_VABufferType(obj_buffer->type),
          obj_context->vdp_codec ? string_of_VdpCodec(obj_context->vdp_codec) : NULL));
    return 0;
}


/* ====================================================================== */
/* === VA API Implementation with VDPAU                               === */
/* ====================================================================== */

static inline int get_num_ref_frames(object_context_p obj_context)
{
    if (obj_context->vdp_codec == VDP_CODEC_H264)
        return obj_context->vdp_picture_info.h264.num_ref_frames;
    return 2;
}

static VdpStatus ensure_decoder_with_max_refs(vdpau_driver_data_t *driver_data,
                                              object_context_p     obj_context,
                                              int                  max_ref_frames)
{
    if (max_ref_frames < 0)
        max_ref_frames = get_VdpDecoder_max_references(obj_context->vdp_profile,
                                                       obj_context->picture_width,
                                                       obj_context->picture_height);

    if (obj_context->vdp_decoder == VDP_INVALID_HANDLE ||
        obj_context->max_ref_frames < max_ref_frames) {
        obj_context->max_ref_frames = max_ref_frames;

        if (obj_context->vdp_decoder != VDP_INVALID_HANDLE) {
            vdpau_decoder_destroy(driver_data, obj_context->vdp_decoder);
            obj_context->vdp_decoder = VDP_INVALID_HANDLE;
        }

        return vdpau_decoder_create(driver_data,
                                    driver_data->vdp_device,
                                    obj_context->vdp_profile,
                                    obj_context->picture_width,
                                    obj_context->picture_height,
                                    max_ref_frames,
                                    &obj_context->vdp_decoder);
    }
    return VDP_STATUS_OK;
}

// Destroy flip queue
static void destroy_flip_queue(vdpau_driver_data_t *driver_data,
                               object_output_p      obj_output)
{
    if (obj_output->vdp_flip_queue != VDP_INVALID_HANDLE) {
        vdpau_presentation_queue_destroy(driver_data,
                                         obj_output->vdp_flip_queue);
        obj_output->vdp_flip_queue = VDP_INVALID_HANDLE;
    }

    if (obj_output->vdp_flip_target != VDP_INVALID_HANDLE) {
        vdpau_presentation_queue_target_destroy(driver_data,
                                                obj_output->vdp_flip_target);
        obj_output->vdp_flip_target = VDP_INVALID_HANDLE;
    }
}

// Create flip queue
static VAStatus create_flip_queue(vdpau_driver_data_t *driver_data,
                                  object_output_p      obj_output)
{
    VdpPresentationQueue vdp_flip_queue = VDP_INVALID_HANDLE;
    VdpPresentationQueueTarget vdp_flip_target = VDP_INVALID_HANDLE;
    VdpStatus vdp_status;

    vdp_status =
        vdpau_presentation_queue_target_create_x11(driver_data,
                                                   driver_data->vdp_device,
                                                   obj_output->drawable,
                                                   &vdp_flip_target);

    if (vdp_status != VDP_STATUS_OK)
        return vdpau_translate_VAStatus(driver_data, vdp_status);

    vdp_status =
        vdpau_presentation_queue_create(driver_data,
                                        driver_data->vdp_device,
                                        vdp_flip_target,
                                        &vdp_flip_queue);

    if (vdp_status != VDP_STATUS_OK) {
        vdpau_presentation_queue_target_destroy(driver_data, vdp_flip_target);
        return vdpau_translate_VAStatus(driver_data, vdp_status);
    }

    obj_output->vdp_flip_queue  = vdp_flip_queue;
    obj_output->vdp_flip_target = vdp_flip_target;
    return VA_STATUS_SUCCESS;
}

static void destroy_output_surface(vdpau_driver_data_t *driver_data, VASurfaceID surface)
{
    if (surface == VA_INVALID_SURFACE)
        return;

    object_output_p obj_output = OUTPUT(surface);
    ASSERT(obj_output);
    if (obj_output == NULL)
        return;

    destroy_flip_queue(driver_data, obj_output);

    int i;
    for (i = 0; i < VDPAU_MAX_OUTPUT_SURFACES; i++) {
        VdpOutputSurface vdp_output_surface = obj_output->vdp_output_surfaces[i];
        if (vdp_output_surface != VDP_INVALID_HANDLE) {
            vdpau_output_surface_destroy(driver_data, vdp_output_surface);
            obj_output->vdp_output_surfaces[i] = VDP_INVALID_HANDLE;
        }
    }

#if USE_GLX
    VADriverContextP const ctx = driver_data->va_context;
    opengl_data_t * const gl_data = &driver_data->gl_data;

    /* XXX: unbind pixmap */

    if (obj_output->fbo_texture) {
        glDeleteTextures(1, &obj_output->fbo_texture);
        obj_output->fbo_texture = 0;
    }

    if (obj_output->fbo_buffer) {
        gl_data->gl_delete_renderbuffers(1, &obj_output->fbo_buffer);
        obj_output->fbo_buffer = 0;
    }

    if (obj_output->fbo) {
        gl_data->gl_delete_framebuffers(1, &obj_output->fbo);
        obj_output->fbo = 0;
    }

    if (obj_output->glx_pixmap) {
        glXDestroyPixmap(ctx->x11_dpy, obj_output->glx_pixmap);
        obj_output->glx_pixmap = None;
    }

    if (obj_output->pixmap) {
        XFreePixmap(ctx->x11_dpy, obj_output->pixmap);
        obj_output->pixmap = None;
    }
#endif

    object_heap_free(&driver_data->output_heap, (object_base_p)obj_output);
}

static VASurfaceID create_output_surface(vdpau_driver_data_t *driver_data,
                                         uint32_t             width,
                                         uint32_t             height)
{
    int i;
    int surface = object_heap_allocate(&driver_data->output_heap);
    if (surface < 0)
        return VA_INVALID_SURFACE;

    object_output_p obj_output = OUTPUT(surface);
    ASSERT(obj_output);
    if (obj_output == NULL)
        return VA_INVALID_SURFACE;

    obj_output->drawable                = None;
    obj_output->width                   = 0;
    obj_output->height                  = 0;
    obj_output->vdp_flip_queue          = VDP_INVALID_HANDLE;
    obj_output->vdp_flip_target         = VDP_INVALID_HANDLE;
    obj_output->output_surface_width    = width;
    obj_output->output_surface_height   = height;
    obj_output->current_output_surface  = 0;
    for (i = 0; i < VDPAU_MAX_OUTPUT_SURFACES; i++)
        obj_output->vdp_output_surfaces[i] = VDP_INVALID_HANDLE;
#if USE_GLX
    obj_output->is_bound                = 0;
    obj_output->pixmap                  = None;
    obj_output->glx_pixmap              = None;
    obj_output->fbo                     = 0;
    obj_output->fbo_buffer              = 0;
    obj_output->fbo_texture             = 0;
#endif

    VADriverContextP const ctx = driver_data->va_context;
    uint32_t display_width, display_height;
    display_width  = DisplayWidth(ctx->x11_dpy, ctx->x11_screen);
    display_height = DisplayHeight(ctx->x11_dpy, ctx->x11_screen);
    if (obj_output->output_surface_width < display_width)
        obj_output->output_surface_width = display_width;
    if (obj_output->output_surface_height < display_height)
        obj_output->output_surface_height = display_height;

    for (i = 0; i < VDPAU_MAX_OUTPUT_SURFACES; i++) {
        VdpStatus vdp_status;
        VdpOutputSurface vdp_output_surface;
        vdp_status = vdpau_output_surface_create(driver_data,
                                                 driver_data->vdp_device,
                                                 VDP_RGBA_FORMAT_B8G8R8A8,
                                                 obj_output->output_surface_width,
                                                 obj_output->output_surface_height,
                                                 &vdp_output_surface);

        if (vdp_status != VDP_STATUS_OK) {
            destroy_output_surface(driver_data, surface);
            return VA_INVALID_SURFACE;
        }

        obj_output->vdp_output_surfaces[i] = vdp_output_surface;
    }

    return surface;
}

// vaQueryConfigProfiles
static VAStatus
vdpau_QueryConfigProfiles(VADriverContextP ctx,
                          VAProfile *profile_list,      /* out */
                          int *num_profiles             /* out */)
{
    INIT_DRIVER_DATA;

    static const VAProfile va_profiles[] = {
        VAProfileMPEG2Simple,
        VAProfileMPEG2Main,
        VAProfileH264Baseline,
        VAProfileH264Main,
        VAProfileH264High,
        VAProfileVC1Simple,
        VAProfileVC1Main,
        VAProfileVC1Advanced
    };
    int i, n = 0;
    for (i = 0; i < ARRAY_ELEMS(va_profiles); i++) {
        VAProfile profile = va_profiles[i];
        if (vdpau_is_supported_profile(driver_data, profile))
            profile_list[n++] = profile;
    }

    /* If the assert fails then VDPAU_MAX_PROFILES needs to be bigger */
    ASSERT(n <= VDPAU_MAX_PROFILES);
    if (num_profiles)
        *num_profiles = n;

    return VA_STATUS_SUCCESS;
}

// vaQueryConfigEntrypoints
static VAStatus
vdpau_QueryConfigEntrypoints(VADriverContextP ctx,
                             VAProfile profile,
                             VAEntrypoint *entrypoint_list,     /* out */
                             int *num_entrypoints               /* out */)
{
    VAEntrypoint entrypoint;

    switch (profile) {
    case VAProfileMPEG2Simple:
    case VAProfileMPEG2Main:
        entrypoint = VAEntrypointVLD;
        break;
    case VAProfileH264Baseline:
    case VAProfileH264Main:
    case VAProfileH264High:
        entrypoint = VAEntrypointVLD;
        break;
    case VAProfileVC1Simple:
    case VAProfileVC1Main:
    case VAProfileVC1Advanced:
        entrypoint = VAEntrypointVLD;
        break;
    default:
        entrypoint = 0;
        break;
    }

    if (entrypoint_list)
        *entrypoint_list = entrypoint;
    if (num_entrypoints)
        *num_entrypoints = entrypoint != 0;

    return VA_STATUS_SUCCESS;
}

// vaGetConfigAttributes
static VAStatus
vdpau_GetConfigAttributes(VADriverContextP ctx,
                          VAProfile profile,
                          VAEntrypoint entrypoint,
                          VAConfigAttrib *attrib_list,  /* in/out */
                          int num_attribs)
{
    int i;

    for (i = 0; i < num_attribs; i++) {
        switch (attrib_list[i].type) {
        case VAConfigAttribRTFormat:
            attrib_list[i].value = VA_RT_FORMAT_YUV420;
            break;
        default:
            attrib_list[i].value = VA_ATTRIB_NOT_SUPPORTED;
            break;
        }
    }

    return VA_STATUS_SUCCESS;
}

static VAStatus
vdpau_update_attribute(object_config_p obj_config, VAConfigAttrib *attrib)
{
    int i;

    /* Check existing attrbiutes */
    for (i = 0; obj_config->attrib_count < i; i++) {
        if (obj_config->attrib_list[i].type == attrib->type) {
            /* Update existing attribute */
            obj_config->attrib_list[i].value = attrib->value;
            return VA_STATUS_SUCCESS;
        }
    }
    if (obj_config->attrib_count < VDPAU_MAX_CONFIG_ATTRIBUTES) {
        i = obj_config->attrib_count;
        obj_config->attrib_list[i].type = attrib->type;
        obj_config->attrib_list[i].value = attrib->value;
        obj_config->attrib_count++;
        return VA_STATUS_SUCCESS;
    }
    return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
}

// vaDestroyConfig
static VAStatus
vdpau_DestroyConfig(VADriverContextP ctx, VAConfigID config_id)
{
    INIT_DRIVER_DATA;

    object_config_p obj_config = CONFIG(config_id);
    ASSERT(obj_config);
    if (obj_config == NULL)
        return VA_STATUS_ERROR_INVALID_CONFIG;

    object_heap_free(&driver_data->config_heap, (object_base_p)obj_config);
    return VA_STATUS_SUCCESS;
}

// vaCreateConfig
static VAStatus
vdpau_CreateConfig(VADriverContextP ctx,
                   VAProfile profile,
                   VAEntrypoint entrypoint,
                   VAConfigAttrib *attrib_list,
                   int num_attribs,
                   VAConfigID *config_id)               /* out */
{
    INIT_DRIVER_DATA;

    VAStatus va_status;
    int configID;
    object_config_p obj_config;
    int i;

    /* Validate profile and entrypoint */
    switch (profile) {
    case VAProfileMPEG2Simple:
    case VAProfileMPEG2Main:
        if (entrypoint == VAEntrypointVLD)
            va_status = VA_STATUS_SUCCESS;
        else
            va_status = VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
        break;
    case VAProfileH264Baseline:
    case VAProfileH264Main:
    case VAProfileH264High:
        if (entrypoint == VAEntrypointVLD)
            va_status = VA_STATUS_SUCCESS;
        else
            va_status = VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
        break;
    case VAProfileVC1Simple:
    case VAProfileVC1Main:
    case VAProfileVC1Advanced:
        if (entrypoint == VAEntrypointVLD)
            va_status = VA_STATUS_SUCCESS;
        else
            va_status = VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
        break;
    default:
        va_status = VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
        break;
    }

    if (va_status != VA_STATUS_SUCCESS)
        return va_status;

    configID = object_heap_allocate(&driver_data->config_heap);
    if ((obj_config = CONFIG(configID)) == NULL)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;

    obj_config->profile = profile;
    obj_config->entrypoint = entrypoint;
    obj_config->attrib_list[0].type = VAConfigAttribRTFormat;
    obj_config->attrib_list[0].value = VA_RT_FORMAT_YUV420;
    obj_config->attrib_count = 1;

    for(i = 0; i < num_attribs; i++) {
        va_status = vdpau_update_attribute(obj_config, &attrib_list[i]);
        if (va_status != VA_STATUS_SUCCESS) {
            vdpau_DestroyConfig(ctx, configID);
            return va_status;
        }
    }

    if (config_id)
        *config_id = configID;

    return va_status;
}

// vaQueryConfigAttributes
static VAStatus
vdpau_QueryConfigAttributes(VADriverContextP ctx,
                            VAConfigID config_id,
                            VAProfile *profile,         /* out */
                            VAEntrypoint *entrypoint,   /* out */
                            VAConfigAttrib *attrib_list,/* out */
                            int *num_attribs)           /* out */
{
    INIT_DRIVER_DATA;

    VAStatus va_status = VA_STATUS_SUCCESS;
    object_config_p obj_config;
    int i;

    obj_config = CONFIG(config_id);
    ASSERT(obj_config);
    if (obj_config == NULL)
        return VA_STATUS_ERROR_INVALID_CONFIG;

    if (profile)
        *profile = obj_config->profile;

    if (entrypoint)
        *entrypoint = obj_config->entrypoint;

    if (num_attribs)
        *num_attribs =  obj_config->attrib_count;

    if (attrib_list) {
        for (i = 0; i < obj_config->attrib_count; i++)
            attrib_list[i] = obj_config->attrib_list[i];
    }

    return va_status;
}

// vaDestroySurfaces
static VAStatus
vdpau_DestroySurfaces(VADriverContextP ctx,
                      VASurfaceID *surface_list,
                      int num_surfaces)
{
    INIT_DRIVER_DATA;

    int i;
    for (i = num_surfaces - 1; i >= 0; i--) {
        object_surface_p obj_surface = SURFACE(surface_list[i]);
        ASSERT(obj_surface);
        if (obj_surface->vdp_surface != VDP_INVALID_HANDLE) {
            vdpau_video_surface_destroy(driver_data, obj_surface->vdp_surface);
            obj_surface->vdp_surface = VDP_INVALID_HANDLE;
        }
        object_heap_free(&driver_data->surface_heap, (object_base_p)obj_surface);
    }
    return VA_STATUS_SUCCESS;
}

// vaCreateSurfaces
static VAStatus
vdpau_CreateSurfaces(VADriverContextP ctx,
                     int width,
                     int height,
                     int format,
                     int num_surfaces,
                     VASurfaceID *surfaces)             /* out */
{
    INIT_DRIVER_DATA;

    VAStatus va_status = VA_STATUS_SUCCESS;
    VdpVideoSurface vdp_surface = VDP_INVALID_HANDLE;
    VdpStatus vdp_status;
    int i;

    /* We only support one format */
    if (format != VA_RT_FORMAT_YUV420)
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    driver_data->vdp_chroma_format = get_VdpChromaType(format);

    for (i = 0; i < num_surfaces; i++) {
        vdp_status = vdpau_video_surface_create(driver_data,
                                                driver_data->vdp_device,
                                                driver_data->vdp_chroma_format,
                                                width, height,
                                                &vdp_surface);
        if (vdp_status != VDP_STATUS_OK) {
            va_status = VA_STATUS_ERROR_ALLOCATION_FAILED;
            break;
        }

        int va_surface = object_heap_allocate(&driver_data->surface_heap);
        object_surface_p obj_surface;
        if ((obj_surface = SURFACE(va_surface)) == NULL) {
            va_status = VA_STATUS_ERROR_ALLOCATION_FAILED;
            break;
        }
        obj_surface->va_context         = 0;
        obj_surface->va_surface_status  = VASurfaceReady;
        obj_surface->vdp_surface        = vdp_surface;
        obj_surface->vdp_output_surface = VDP_INVALID_HANDLE;
        obj_surface->width              = width;
        obj_surface->height             = height;
        surfaces[i]                     = va_surface;
        vdp_surface                     = VDP_INVALID_HANDLE;
    }

    /* Error recovery */
    if (va_status != VA_STATUS_SUCCESS) {
        if (vdp_surface != VDP_INVALID_HANDLE)
            vdpau_video_surface_destroy(driver_data, vdp_surface);
        vdpau_DestroySurfaces(ctx, surfaces, i);
    }

    return va_status;
}

// vaDestroyContext
static VAStatus
vdpau_DestroyContext(VADriverContextP ctx,
                     VAContextID context)
{
    INIT_DRIVER_DATA;
    int i;

    object_context_p obj_context = CONTEXT(context);
    ASSERT(obj_context);
    if (obj_context == NULL)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    if (obj_context->vdp_bitstream_buffers) {
        free(obj_context->vdp_bitstream_buffers);
        obj_context->vdp_bitstream_buffers = NULL;
        obj_context->vdp_bitstream_buffers_count_max = 0;
    }

    if (obj_context->vdp_video_surfaces) {
        for (i = 0; i < obj_context->num_render_targets; i++) {
            VdpVideoSurface surface = obj_context->vdp_video_surfaces[i];
            if (surface != VDP_INVALID_HANDLE) {
                vdpau_video_surface_destroy(driver_data, surface);
                obj_context->vdp_video_surfaces[i] = VDP_INVALID_HANDLE;
            }
        }
        free(obj_context->vdp_video_surfaces);
        obj_context->vdp_video_surfaces = NULL;
    }

    if (obj_context->vdp_video_mixer != VDP_INVALID_HANDLE) {
        vdpau_video_mixer_destroy(driver_data, obj_context->vdp_video_mixer);
        obj_context->vdp_video_mixer = VDP_INVALID_HANDLE;
    }

    if (obj_context->vdp_decoder != VDP_INVALID_HANDLE) {
        vdpau_decoder_destroy(driver_data, obj_context->vdp_decoder);
        obj_context->vdp_decoder = VDP_INVALID_HANDLE;
    }

    if (obj_context->dead_buffers) {
        free(obj_context->dead_buffers);
        obj_context->dead_buffers = NULL;
    }

    if (obj_context->render_targets) {
        free(obj_context->render_targets);
        obj_context->render_targets = NULL;
    }

    if (obj_context->output_surface != VA_INVALID_SURFACE) {
        destroy_output_surface(driver_data, obj_context->output_surface);
        obj_context->output_surface = VA_INVALID_SURFACE;
    }

    obj_context->context_id             = -1;
    obj_context->config_id              = -1;
    obj_context->current_render_target  = VA_INVALID_SURFACE;
    obj_context->picture_width          = 0;
    obj_context->picture_height         = 0;
    obj_context->num_render_targets     = 0;
    obj_context->flags                  = 0;
    obj_context->dead_buffers_count     = 0;
    obj_context->dead_buffers_count_max = 0;

    object_heap_free(&driver_data->context_heap, (object_base_p)obj_context);
    return VA_STATUS_SUCCESS;
}

// vaCreateContext
static VAStatus
vdpau_CreateContext(VADriverContextP ctx,
                    VAConfigID config_id,
                    int picture_width,
                    int picture_height,
                    int flag,
                    VASurfaceID *render_targets,
                    int num_render_targets,
                    VAContextID *context)               /* out */
{
    INIT_DRIVER_DATA;

    if (context)
        *context = 0;

    object_config_p obj_config;
    if ((obj_config = CONFIG(config_id)) == NULL)
        return VA_STATUS_ERROR_INVALID_CONFIG;

    /* XXX: validate flag */

    VdpDecoderProfile vdp_profile;
    uint32_t max_width, max_height;
    vdp_profile = get_VdpDecoderProfile(obj_config->profile);
    if (!vdpau_get_surface_size_max(driver_data, vdp_profile, &max_width, &max_height))
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    if (picture_width > max_width || picture_height > max_height)
        return VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED;

    int contextID = object_heap_allocate(&driver_data->context_heap);
    object_context_p obj_context;
    if ((obj_context = CONTEXT(contextID)) == NULL)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    if (context)
        *context = contextID;

    obj_context->context_id             = contextID;
    obj_context->config_id              = config_id;
    obj_context->current_render_target  = VA_INVALID_SURFACE;
    obj_context->picture_width          = picture_width;
    obj_context->picture_height         = picture_height;
    obj_context->num_render_targets     = num_render_targets;
    obj_context->flags                  = flag;
    obj_context->max_ref_frames         = -1;
    obj_context->output_surface         =
        create_output_surface(driver_data, picture_width, picture_height);
    obj_context->render_targets         = (VASurfaceID *)
        calloc(num_render_targets, sizeof(VASurfaceID));
    obj_context->dead_buffers           = NULL;
    obj_context->dead_buffers_count     = 0;
    obj_context->dead_buffers_count_max = 0;
    obj_context->vdp_codec              = get_VdpCodec(vdp_profile);
    obj_context->vdp_profile            = vdp_profile;
    obj_context->vdp_decoder            = VDP_INVALID_HANDLE;
    obj_context->vdp_video_mixer        = VDP_INVALID_HANDLE;
    obj_context->vdp_video_surfaces     = (VdpVideoSurface *)
        calloc(num_render_targets, sizeof(VdpVideoSurface));
    obj_context->vdp_bitstream_buffers = NULL;
    obj_context->vdp_bitstream_buffers_count = 0;
    obj_context->vdp_bitstream_buffers_count_max = 0;

    vdpau_h264_clear_reference_frames(obj_context);

    if (obj_context->output_surface == VA_INVALID_SURFACE) {
        vdpau_DestroyContext(ctx, contextID);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    if (obj_context->render_targets == NULL || obj_context->vdp_video_surfaces == NULL) {
        vdpau_DestroyContext(ctx, contextID);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    int i;
    for (i = 0; i < num_render_targets; i++) {
        object_surface_t *obj_surface;
        if ((obj_surface = SURFACE(render_targets[i])) == NULL) {
            vdpau_DestroyContext(ctx, contextID);
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        obj_context->render_targets[i] = render_targets[i];
        obj_context->vdp_video_surfaces[i] = obj_surface->vdp_surface;
        /* XXX: assume we can only associate a surface to a single context */
        ASSERT(obj_surface->va_context == 0);
        obj_surface->va_context = contextID;
    }

    VdpVideoMixerParameter params[] = {
        VDP_VIDEO_MIXER_PARAMETER_VIDEO_SURFACE_WIDTH,
        VDP_VIDEO_MIXER_PARAMETER_VIDEO_SURFACE_HEIGHT,
        VDP_VIDEO_MIXER_PARAMETER_CHROMA_TYPE
    };

    const void *param_values[3];
    param_values[0] = &picture_width;
    param_values[1] = &picture_height;
    param_values[2] = &driver_data->vdp_chroma_format;

    VdpStatus vdp_status;
    vdp_status = vdpau_video_mixer_create(driver_data,
                                          driver_data->vdp_device,
                                          0,
                                          NULL,
                                          ARRAY_ELEMS(params),
                                          params,
                                          param_values,
                                          &obj_context->vdp_video_mixer);
    if (vdp_status != VDP_STATUS_OK) {
        vdpau_DestroyContext(ctx, contextID);
        return vdpau_translate_VAStatus(driver_data, vdp_status);
    }

    return VA_STATUS_SUCCESS;
}

static inline int
vdpau_allocate_buffer(object_buffer_p obj_buffer, int size)
{
    obj_buffer->buffer_data = realloc(obj_buffer->buffer_data, size);
    return obj_buffer->buffer_data != NULL;
}

static void
vdpau_destroy_buffer(vdpau_driver_data_t *driver_data,
                     object_buffer_p      obj_buffer)
{
    if (obj_buffer->buffer_data) {
        free(obj_buffer->buffer_data);
        obj_buffer->buffer_data = NULL;
    }
    object_heap_free(&driver_data->buffer_heap, (object_base_p)obj_buffer);
}

static void
vdpau_schedule_destroy_buffer(object_context_p obj_context,
                              object_buffer_p  obj_buffer)
{
    if (obj_context->dead_buffers_count >= obj_context->dead_buffers_count_max) {
        obj_context->dead_buffers_count_max += 16;
        obj_context->dead_buffers = realloc(obj_context->dead_buffers,
                                            obj_context->dead_buffers_count_max * sizeof(obj_context->dead_buffers[0]));
        ASSERT(obj_context->dead_buffers);
    }
    ASSERT(obj_context->dead_buffers);
    obj_context->dead_buffers[obj_context->dead_buffers_count] = obj_buffer->base.id;
    obj_context->dead_buffers_count++;
}

// vaDestroyBuffer
static VAStatus
vdpau_DestroyBuffer(VADriverContextP ctx,
                    VABufferID buffer_id)
{
    INIT_DRIVER_DATA;

    object_buffer_p obj_buffer = BUFFER(buffer_id);
#if 1
    if (obj_buffer)
#else
    ASSERT(obj_buffer);
    if (obj_buffer == NULL)
        return VA_STATUS_ERROR_INVALID_BUFFER;
#endif

    vdpau_destroy_buffer(driver_data, obj_buffer);
    return VA_STATUS_SUCCESS;
}

// vaCreateBuffer
static VAStatus
vdpau_CreateBuffer(VADriverContextP ctx,
                   VAContextID context,         /* in */
                   VABufferType type,           /* in */
                   unsigned int size,           /* in */
                   unsigned int num_elements,   /* in */
                   void *data,                  /* in */
                   VABufferID *buf_id)          /* out */
{
    INIT_DRIVER_DATA;

    /* Validate type */
    switch (type) {
    case VAPictureParameterBufferType:
    case VAIQMatrixBufferType:
    case VASliceParameterBufferType:
    case VASliceDataBufferType:
    case VABitPlaneBufferType:
    case VAImageBufferType:
        /* Ok */
        break;
    default:
        D(bug("ERROR: unsupported buffer type %d\n", type));
        return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
    }

    int bufferID = object_heap_allocate(&driver_data->buffer_heap);
    object_buffer_p obj_buffer;
    if ((obj_buffer = BUFFER(bufferID)) == NULL)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;

    obj_buffer->buffer_data = NULL;
    obj_buffer->buffer_size = size * num_elements;
    if (!vdpau_allocate_buffer(obj_buffer, obj_buffer->buffer_size)) {
        vdpau_DestroyBuffer(ctx, bufferID);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    obj_buffer->type = type;
    obj_buffer->max_num_elements = num_elements;
    obj_buffer->num_elements = num_elements;
    if (data)
        memcpy(obj_buffer->buffer_data, data, obj_buffer->buffer_size);

    if (buf_id)
        *buf_id = bufferID;

    return VA_STATUS_SUCCESS;
}

// vaBufferSetNumElements
static VAStatus
vdpau_BufferSetNumElements(VADriverContextP ctx,
                           VABufferID buf_id,           /* in */
                           unsigned int num_elements)   /* in */
{
    INIT_DRIVER_DATA;

    object_buffer_p obj_buffer = BUFFER(buf_id);
    ASSERT(obj_buffer);
    if (obj_buffer == NULL)
        return VA_STATUS_ERROR_INVALID_BUFFER;

    if (num_elements < 0 || num_elements > obj_buffer->max_num_elements)
        return VA_STATUS_ERROR_UNKNOWN;

    obj_buffer->num_elements = num_elements;
    return VA_STATUS_SUCCESS;
}

// vaMapBuffer
static VAStatus
vdpau_MapBuffer(VADriverContextP ctx,
                VABufferID buf_id,              /* in */
                void **pbuf)                    /* out */
{
    INIT_DRIVER_DATA;

    object_buffer_p obj_buffer = BUFFER(buf_id);
    ASSERT(obj_buffer);
    if (obj_buffer == NULL)
        return VA_STATUS_ERROR_INVALID_BUFFER;

    if (pbuf)
        *pbuf = obj_buffer->buffer_data;

    if (obj_buffer->buffer_data == NULL)
        return VA_STATUS_ERROR_UNKNOWN;

    return VA_STATUS_SUCCESS;
}

// vaUnmapBuffer
static VAStatus
vdpau_UnmapBuffer(VADriverContextP ctx,
                  VABufferID buf_id)            /* in */
{
    /* Don't do anything there, translate structure in vaRenderPicture() */
    return VA_STATUS_SUCCESS;
}

// vaQueryImageFormats
static VAStatus
vdpau_QueryImageFormats(VADriverContextP ctx,
                        VAImageFormat *format_list,     /* out */
                        int *num_formats)               /* out */
{
    INIT_DRIVER_DATA;

    if (num_formats)
        *num_formats = 0;

    if (format_list == NULL)
        return VA_STATUS_SUCCESS;

    int i, n = 0;
    for (i = 0; i < ARRAY_ELEMS(vdpau_image_formats_map); i++) {
        const vdpau_image_format_map_t * const f = &vdpau_image_formats_map[i];
        if (vdpau_is_supported_image_format(driver_data, f->type, f->format))
            format_list[n++] = f->va_format;
    }

    /* If the assert fails then VDPAU_MAX_IMAGE_FORMATS needs to be bigger */
    ASSERT(n <= VDPAU_MAX_IMAGE_FORMATS);
    if (num_formats)
        *num_formats = n;

    return VA_STATUS_SUCCESS;
}

// vaDestroyImage
static VAStatus
vdpau_DestroyImage(VADriverContextP ctx,
                   VAImageID image_id)
{
    INIT_DRIVER_DATA;

    VAImage *image;
    object_image_p obj_image;

    if ((obj_image = IMAGE(image_id)) == NULL)
        return VA_STATUS_ERROR_INVALID_IMAGE;
    if ((image = obj_image->image) == NULL)
        return VA_STATUS_ERROR_INVALID_IMAGE;

    if (obj_image->vdp_rgba_surface != VDP_INVALID_HANDLE)
        vdpau_output_surface_destroy(driver_data, obj_image->vdp_rgba_surface);

    object_heap_free(&driver_data->image_heap, (object_base_p)obj_image);
    return vdpau_DestroyBuffer(ctx, image->buf);
}

// vaCreateImage
static VAStatus
vdpau_CreateImage(VADriverContextP ctx,
                  VAImageFormat *format,
                  int width,
                  int height,
                  VAImage *image)                       /* out */
{
    INIT_DRIVER_DATA;

    VdpRGBAFormat vdp_rgba_format;
    VAStatus va_status = VA_STATUS_ERROR_OPERATION_FAILED;
    int image_id;
    object_image_p obj_image = NULL;

    if (format == NULL)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    if (image == NULL)
        return VA_STATUS_ERROR_INVALID_IMAGE;
    image->image_id             = 0;
    image->buf                  = 0;

    if ((image_id = object_heap_allocate(&driver_data->image_heap)) < 0)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    image->image_id = image_id;

    if ((obj_image = IMAGE(image_id)) == NULL)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    obj_image->image = image;
    obj_image->vdp_rgba_surface = VDP_INVALID_HANDLE;

    switch (format->fourcc) {
    case VA_FOURCC('N','V','1','2'):
        image->num_planes       = 2;
        image->pitches[0]       = width;
        image->offsets[0]       = 0;
        image->pitches[1]       = width;
        image->offsets[1]       = image->offsets[0] + image->pitches[0] * height;
        image->data_size        = image->offsets[1] + image->pitches[1] * ((height + 1) / 2);
        break;
    case VA_FOURCC('Y','V','1','2'):
        image->num_planes       = 3;
        image->pitches[0]       = width;
        image->offsets[0]       = 0;
        image->pitches[1]       = (width + 1) / 2;
        image->offsets[1]       = image->offsets[0] + image->pitches[0] * height;
        image->pitches[2]       = (width + 1) / 2;
        image->offsets[2]       = image->offsets[1] + image->pitches[1] * ((height + 1) / 2);
        image->data_size        = image->offsets[2] + image->pitches[2] * ((height + 1) / 2);
        break;
    case VA_FOURCC('R','G','B','A'):
        if ((vdp_rgba_format = get_VdpRGBAFormat(format)) == (VdpRGBAFormat)-1)
            goto error;
        if (vdpau_output_surface_create(driver_data,
                                        driver_data->vdp_device,
                                        vdp_rgba_format, width, height,
                                        &obj_image->vdp_rgba_surface) != VDP_STATUS_OK)
            goto error;
        // fall-through
    case VA_FOURCC('U','Y','V','Y'):
    case VA_FOURCC('Y','U','Y','V'):
        image->num_planes       = 1;
        image->pitches[0]       = width * 4;
        image->offsets[0]       = 0;
        image->data_size        = image->offsets[0] + image->pitches[0] * height;
        break;
    default:
        goto error;
    }

    va_status = vdpau_CreateBuffer(ctx, 0, VAImageBufferType,
                                   image->data_size, 1, NULL,
                                   &image->buf);
    if (va_status != VA_STATUS_SUCCESS)
        goto error;

    obj_image->image            = image;
    image->image_id             = image_id;
    image->format               = *format;
    image->width                = width;
    image->height               = height;

    /* XXX: no paletted formats supported yet */
    image->num_palette_entries  = 0;
    image->entry_bytes          = 0;
    return VA_STATUS_SUCCESS;

 error:
    vdpau_DestroyImage(ctx, image_id);
    return va_status;
}

// vaDeriveImage
static VAStatus
vdpau_DeriveImage(VADriverContextP ctx,
                  VASurfaceID surface,
                  VAImage *image)                       /* out */
{
    /* TODO */
    return VA_STATUS_ERROR_OPERATION_FAILED;
}

// vaSetImagePalette
static VAStatus
vdpau_SetImagePalette(VADriverContextP ctx,
                      VAImageID image,
                      unsigned char *palette)
{
    /* TODO */
    return VA_STATUS_ERROR_OPERATION_FAILED;
}

// vaGetImage
static VAStatus
vdpau_GetImage(VADriverContextP ctx,
               VASurfaceID surface,
               int x,     /* coordinates of the upper left source pixel */
               int y,
               unsigned int width, /* width and height of the region */
               unsigned int height,
               VAImageID image_id)
{
    INIT_DRIVER_DATA;

    object_context_p obj_context;
    object_buffer_p obj_buffer;
    object_surface_p obj_surface;
    object_image_p obj_image;
    VAImage *image;
    VdpStatus vdp_status;
    VdpRGBAFormat rgba_format;
    VdpYCbCrFormat ycbcr_format;
    VdpRect r;
    uint8_t *src[3];
    unsigned int src_stride[3];
    int i, is_full_surface, is_yuv_format;

    if ((obj_surface = SURFACE(surface)) == NULL)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    /* XXX: only support full surface readback for now */
    is_full_surface = (x == 0 &&
                       y == 0 &&
                       obj_surface->width == width &&
                       obj_surface->height == height);
    if (!is_full_surface)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    if ((obj_image = IMAGE(image_id)) == NULL)
        return VA_STATUS_ERROR_INVALID_IMAGE;
    if ((image = obj_image->image) == NULL)
        return VA_STATUS_ERROR_INVALID_IMAGE;

    if ((obj_buffer = BUFFER(image->buf)) == NULL)
        return VA_STATUS_ERROR_INVALID_BUFFER;

    is_yuv_format = obj_image->vdp_rgba_surface == VDP_INVALID_HANDLE;
    if (is_yuv_format) {
        if ((ycbcr_format = get_VdpYCbCrFormat(&image->format)) == (VdpYCbCrFormat)-1)
            return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    else {
        if ((rgba_format = get_VdpRGBAFormat(&image->format)) == (VdpRGBAFormat)-1)
            return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    for (i = 0; i < image->num_planes; i++) {
        src[i] = (uint8_t *)obj_buffer->buffer_data + image->offsets[i];
        src_stride[i] = image->pitches[i];
    }

    if (is_yuv_format) {
        vdp_status = vdpau_video_surface_get_bits_ycbcr(driver_data,
                                                        obj_surface->vdp_surface,
                                                        ycbcr_format,
                                                        src, src_stride);
    }
    else {
        if ((obj_context = CONTEXT(obj_surface->va_context)) == NULL)
            return VA_STATUS_ERROR_INVALID_CONTEXT;

        r.x0 = x;
        r.y0 = y;
        r.x1 = x + width;
        r.y1 = y + height;
        vdp_status = vdpau_video_mixer_render(driver_data,
                                              obj_context->vdp_video_mixer,
                                              VDP_INVALID_HANDLE, NULL,
                                              VDP_VIDEO_MIXER_PICTURE_STRUCTURE_FRAME,
                                              0, NULL,
                                              obj_surface->vdp_surface,
                                              0, NULL,
                                              &r,
                                              obj_image->vdp_rgba_surface,
                                              &r,
                                              &r,
                                              0, NULL);
        if (vdp_status != VDP_STATUS_OK)
            return vdpau_translate_VAStatus(driver_data, vdp_status);

        vdp_status = vdpau_output_surface_get_bits_native(driver_data,
                                                          obj_image->vdp_rgba_surface,
                                                          &r,
                                                          src,
                                                          src_stride);
    }

    return vdpau_translate_VAStatus(driver_data, vdp_status);
}

// vaPutImage
static VAStatus
vdpau_PutImage(VADriverContextP ctx,
               VASurfaceID surface,
               VAImageID image,
               int src_x,
               int src_y,
               unsigned int width,
               unsigned int height,
               int dest_x,
               int dest_y)
{
    /* TODO */
    return VA_STATUS_ERROR_OPERATION_FAILED;
}

// vaPutImage2
static VAStatus
vdpau_PutImage2(VADriverContextP ctx,
                VASurfaceID surface,
                VAImageID image,
                int src_x,
                int src_y,
                unsigned int src_width,
                unsigned int src_height,
                int dest_x,
                int dest_y,
                unsigned int dest_width,
                unsigned int dest_height)
{
    /* TODO */
    return VA_STATUS_ERROR_OPERATION_FAILED;
}

// vaQuerySubpictureFormats
static VAStatus
vdpau_QuerySubpictureFormats(VADriverContextP ctx,
                             VAImageFormat *format_list,/* out */
                             unsigned int *flags,       /* out */
                             unsigned int *num_formats) /* out */
{
    if (num_formats)
      *num_formats = 0;

    return VA_STATUS_SUCCESS;
}

// vaCreateSubpicture
static VAStatus
vdpau_CreateSubpicture(VADriverContextP ctx,
                       VAImageID image,
                       VASubpictureID *subpicture)      /* out */
{
    /* TODO */
    return VA_STATUS_ERROR_OPERATION_FAILED;
}

// vaDestroySubpicture
static VAStatus
vdpau_DestroySubpicture(VADriverContextP ctx,
                        VASubpictureID subpicture)
{
    /* TODO */
    return VA_STATUS_ERROR_OPERATION_FAILED;
}

// vaSetSubpictureImage
static VAStatus
vdpau_SetSubpictureImage(VADriverContextP ctx,
                         VASubpictureID subpicture,
                         VAImageID image)
{
    /* TODO */
    return VA_STATUS_ERROR_OPERATION_FAILED;
}

// vaSetSubpicturePalette (not a PUBLIC interface)
static VAStatus
vdpau_SetSubpicturePalette(VADriverContextP ctx,
                           VASubpictureID subpicture,
                           unsigned char *palette)
{
    /* TODO */
    return VA_STATUS_ERROR_OPERATION_FAILED;
}

// vaSetSubpictureChromaKey
static VAStatus
vdpau_SetSubpictureChromakey(VADriverContextP ctx,
                             VASubpictureID subpicture,
                             unsigned int chromakey_min,
                             unsigned int chromakey_max,
                             unsigned int chromakey_mask)
{
    /* TODO */
    return VA_STATUS_ERROR_OPERATION_FAILED;
}

// vaSetSubpictureGlobalAlpha
static VAStatus
vdpau_SetSubpictureGlobalAlpha(VADriverContextP ctx,
                               VASubpictureID subpicture,
                               float global_alpha)
{
    /* TODO */
    return VA_STATUS_ERROR_OPERATION_FAILED;
}

// vaAssociateSubpicture
static VAStatus
vdpau_AssociateSubpicture(VADriverContextP ctx,
                          VASubpictureID subpicture,
                          VASurfaceID *target_surfaces,
                          int num_surfaces,
                          short src_x, /* upper left offset in subpicture */
                          short src_y,
                          short dest_x, /* upper left offset in surface */
                          short dest_y,
                          unsigned short width,
                          unsigned short height,
                          unsigned int flags)
{
    /* TODO */
    return VA_STATUS_ERROR_OPERATION_FAILED;
}

// vaAssociateSubpicture2
static VAStatus
vdpau_AssociateSubpicture2(VADriverContextP ctx,
                           VASubpictureID subpicture,
                           VASurfaceID *target_surfaces,
                           int num_surfaces,
                           short src_x, /* upper left offset in subpicture */
                           short src_y,
                           unsigned short src_width,
                           unsigned short src_height,
                           short dest_x, /* upper left offset in surface */
                           short dest_y,
                           unsigned short dest_width,
                           unsigned short dest_height,
                           unsigned int flags)
{
    /* TODO */
    return VA_STATUS_ERROR_OPERATION_FAILED;
}

// vaDeassociateSubpicture
static VAStatus
vdpau_DeassociateSubpicture(VADriverContextP ctx,
                            VASubpictureID subpicture,
                            VASurfaceID *target_surfaces,
                            int num_surfaces)
{
    /* TODO */
    return VA_STATUS_ERROR_OPERATION_FAILED;
}

// vaBeginPicture
static VAStatus
vdpau_BeginPicture(VADriverContextP ctx,
                   VAContextID context,
                   VASurfaceID render_target)
{
    INIT_DRIVER_DATA;

    object_context_p obj_context = CONTEXT(context);
    ASSERT(obj_context);
    if (obj_context == NULL)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    object_surface_p obj_surface = SURFACE(render_target);
    ASSERT(obj_surface);
    if (obj_surface == NULL)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    obj_surface->va_surface_status              = VASurfaceRendering;
    obj_surface->vdp_output_surface             = VDP_INVALID_HANDLE;
    obj_context->last_slice_params              = NULL;
    obj_context->last_slice_params_count        = 0;
    obj_context->current_render_target          = obj_surface->base.id;
    obj_context->vdp_bitstream_buffers_count    = 0;

    switch (obj_context->vdp_codec) {
    case VDP_CODEC_MPEG1:
    case VDP_CODEC_MPEG2:
        obj_context->vdp_picture_info.mpeg2.slice_count = 0;
        break;
    case VDP_CODEC_H264:
        obj_context->vdp_picture_info.h264.slice_count = 0;
        break;
    case VDP_CODEC_VC1:
        obj_context->vdp_picture_info.vc1.slice_count = 0;
        break;
    default:
        assert(0);
        break;
    }
    return VA_STATUS_SUCCESS;
}

// vaRenderPicture
static VAStatus
vdpau_RenderPicture(VADriverContextP ctx,
                    VAContextID context,
                    VABufferID *buffers,
                    int num_buffers)
{
    INIT_DRIVER_DATA;
    int i;

    object_context_p obj_context = CONTEXT(context);
    ASSERT(obj_context);
    if (obj_context == NULL)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    object_surface_p obj_surface = SURFACE(obj_context->current_render_target);
    ASSERT(obj_surface);
    if (obj_surface == NULL)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    /* Verify that we got valid buffer references */
    for (i = 0; i < num_buffers; i++) {
        object_buffer_p obj_buffer = BUFFER(buffers[i]);
        ASSERT(obj_buffer);
        if (obj_buffer == NULL)
            return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    /* Translate buffers */
    for (i = 0; i < num_buffers; i++) {
        object_buffer_p obj_buffer = BUFFER(buffers[i]);
        ASSERT(obj_buffer);
        if (!vdpau_translate_buffer(driver_data, obj_context, obj_buffer))
            return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
        /* Release any buffer that is not VASliceDataBuffer */
        /* VASliceParameterBuffer is also needed to check for start_codes */
        switch (obj_buffer->type) {
        case VASliceParameterBufferType:
        case VASliceDataBufferType:
            vdpau_schedule_destroy_buffer(obj_context, obj_buffer);
            break;
        default:
            vdpau_destroy_buffer(driver_data, obj_buffer);
            break;
        }
    }

    return VA_STATUS_SUCCESS;
}

// vaEndPicture
static VAStatus
vdpau_EndPicture(VADriverContextP ctx,
                 VAContextID context)
{
    INIT_DRIVER_DATA;
    unsigned int i;

    object_context_p obj_context = CONTEXT(context);
    ASSERT(obj_context);
    if (obj_context == NULL)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    vdpau_h264_update_reference_frames(driver_data, obj_context);

    object_surface_p obj_surface = SURFACE(obj_context->current_render_target);
    ASSERT(obj_surface);
    if (obj_surface == NULL)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    object_output_p obj_output = OUTPUT(obj_context->output_surface);
    ASSERT(obj_output);
    if (obj_output == NULL)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    if (trace_enabled()) {
        switch (obj_context->vdp_codec) {
        case VDP_CODEC_MPEG1:
        case VDP_CODEC_MPEG2:
            dump_VdpPictureInfoMPEG1Or2(&obj_context->vdp_picture_info.mpeg2);
            break;
        case VDP_CODEC_H264:
            dump_VdpPictureInfoH264(&obj_context->vdp_picture_info.h264);
            break;
        case VDP_CODEC_VC1:
            dump_VdpPictureInfoVC1(&obj_context->vdp_picture_info.vc1);
            break;
        default:
            break;
        }
        for (i = 0; i < obj_context->vdp_bitstream_buffers_count; i++)
            dump_VdpBitstreamBuffer(&obj_context->vdp_bitstream_buffers[i]);
    }

    VAStatus va_status;
    VdpStatus vdp_status;
    vdp_status = ensure_decoder_with_max_refs(driver_data,
                                              obj_context,
                                              get_num_ref_frames(obj_context));

    if (vdp_status == VDP_STATUS_OK)
        vdp_status = vdpau_decoder_render(driver_data,
                                          obj_context->vdp_decoder,
                                          obj_surface->vdp_surface,
                                          (VdpPictureInfo)&obj_context->vdp_picture_info,
                                          obj_context->vdp_bitstream_buffers_count,
                                          obj_context->vdp_bitstream_buffers);
    va_status = vdpau_translate_VAStatus(driver_data, vdp_status);

    /* XXX: assume we are done with rendering right away */
    obj_context->current_render_target = VA_INVALID_SURFACE;

    /* Release pending buffers */
    if (obj_context->dead_buffers_count > 0) {
        ASSERT(obj_context->dead_buffers);
        int i;
        for (i = 0; i < obj_context->dead_buffers_count; i++) {
            object_buffer_p obj_buffer = BUFFER(obj_context->dead_buffers[i]);
            ASSERT(obj_buffer);
            vdpau_destroy_buffer(driver_data, obj_buffer);
        }
        obj_context->dead_buffers_count = 0;
    }

    return va_status;
}

// vaQuerySurfaceStatus
static VAStatus
query_surface_status(vdpau_driver_data_t *driver_data,
                     object_context_p     obj_context,
                     object_surface_p     obj_surface,
                     VASurfaceStatus     *status)
{
    VAStatus va_status = VA_STATUS_SUCCESS;

    if (obj_surface->va_surface_status == VASurfaceDisplaying &&
        obj_surface->vdp_output_surface != VDP_INVALID_HANDLE) {
        object_output_p obj_output = OUTPUT(obj_context->output_surface);
        ASSERT(obj_output);
        if (obj_output == NULL)
            return VA_STATUS_ERROR_INVALID_SURFACE;

        VdpPresentationQueueStatus vdp_queue_status;
        VdpTime vdp_dummy_time;
        VdpStatus vdp_status =
            vdpau_presentation_queue_query_surface_status(driver_data,
                                                          obj_output->vdp_flip_queue,
                                                          obj_surface->vdp_output_surface,
                                                          &vdp_queue_status,
                                                          &vdp_dummy_time);
        va_status = vdpau_translate_VAStatus(driver_data, vdp_status);

        if (vdp_queue_status == VDP_PRESENTATION_QUEUE_STATUS_VISIBLE) {
            obj_surface->va_surface_status  = VASurfaceReady;
            obj_surface->vdp_output_surface = VDP_INVALID_HANDLE;
        }
    }

    if (status)
        *status = obj_surface->va_surface_status;

    return va_status;
}

static VAStatus
vdpau_QuerySurfaceStatus(VADriverContextP ctx,
                         VASurfaceID render_target,
                         VASurfaceStatus *status)       /* out */
{
    INIT_DRIVER_DATA;

    object_surface_p obj_surface = SURFACE(render_target);
    ASSERT(obj_surface);
    if (obj_surface == NULL)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    object_context_p obj_context = CONTEXT(obj_surface->va_context);
    ASSERT(obj_context);
    if (obj_context == NULL)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    return query_surface_status(driver_data, obj_context, obj_surface, status);
}

// vaSyncSurface
static VAStatus
sync_surface(vdpau_driver_data_t *driver_data,
             object_context_p     obj_context,
             object_surface_p     obj_surface)
{
    /* VDPAU only supports status interface for in-progress display */
    /* XXX: polling is bad but there currently is no alternative */
    for (;;) {
        VASurfaceStatus va_surface_status;
        VAStatus va_status = query_surface_status(driver_data,
                                                  obj_context,
                                                  obj_surface,
                                                  &va_surface_status);

        if (va_status != VA_STATUS_SUCCESS)
            return va_status;

        if (va_surface_status != VASurfaceDisplaying)
            break;
        delay_usec(VDPAU_SYNC_DELAY);
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus
vdpau_SyncSurface(VADriverContextP ctx,
                  VAContextID context,
                  VASurfaceID render_target)
{
    INIT_DRIVER_DATA;

    object_context_p obj_context = CONTEXT(context);
    ASSERT(obj_context);
    if (obj_context == NULL)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    object_surface_p obj_surface = SURFACE(render_target);
    ASSERT(obj_surface);
    if (obj_surface == NULL)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    /* Assume that this shouldn't be called before vaEndPicture() */
    ASSERT(obj_context->current_render_target != obj_surface->base.id);

    return sync_surface(driver_data, obj_context, obj_surface);
}

// vaPutSurface
static VAStatus
put_surface(vdpau_driver_data_t *driver_data,
            VASurfaceID          surface,
            Drawable             drawable,
            unsigned int         drawable_width,
            unsigned int         drawable_height,
            const VdpRect       *source_rect,
            const VdpRect       *target_rect)
{
    object_surface_p obj_surface = SURFACE(surface);
    ASSERT(obj_surface);
    if (obj_surface == NULL)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    object_context_p obj_context = CONTEXT(obj_surface->va_context);
    ASSERT(obj_context);
    if (obj_context == NULL)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    object_output_p obj_output = OUTPUT(obj_context->output_surface);
    ASSERT(obj_output);
    if (obj_output == NULL)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    obj_surface->va_surface_status  = VASurfaceReady;
    obj_surface->vdp_output_surface = VDP_INVALID_HANDLE;

    VdpStatus vdp_status;
    VAStatus va_status;

    if (obj_output->drawable != drawable) {
        destroy_flip_queue(driver_data, obj_output);

        obj_output->drawable = drawable;
        va_status = create_flip_queue(driver_data, obj_output);
        if (va_status != VA_STATUS_SUCCESS)
            return va_status;
    }

    if (obj_output->width  != drawable_width ||
        obj_output->height != drawable_height) {
        obj_output->width   = drawable_width;
        obj_output->height  = drawable_height;

        /* XXX: re-create output surfaces incrementally here? */
    }

    ASSERT(obj_output->drawable == drawable);
    ASSERT(obj_output->vdp_flip_queue != VDP_INVALID_HANDLE);
    ASSERT(obj_output->vdp_flip_target != VDP_INVALID_HANDLE);

    VdpOutputSurface vdp_output_surface;
    vdp_output_surface = obj_output->vdp_output_surfaces[obj_output->current_output_surface];

    VdpTime dummy_time;
    vdp_status = vdpau_presentation_queue_block_until_surface_idle(driver_data,
                                                                   obj_output->vdp_flip_queue,
                                                                   vdp_output_surface,
                                                                   &dummy_time);

    if (vdp_status != VDP_STATUS_OK)
        return vdpau_translate_VAStatus(driver_data, vdp_status);

    vdp_status = vdpau_video_mixer_render(driver_data,
                                          obj_context->vdp_video_mixer,
                                          VDP_INVALID_HANDLE,
                                          NULL,
                                          VDP_VIDEO_MIXER_PICTURE_STRUCTURE_FRAME,
                                          0, NULL,
                                          obj_surface->vdp_surface,
                                          0, NULL,
                                          source_rect,
                                          vdp_output_surface,
                                          NULL,
                                          target_rect,
                                          0, NULL);

    if (vdp_status != VDP_STATUS_OK)
        return vdpau_translate_VAStatus(driver_data, vdp_status);

    uint32_t clip_width, clip_height;
    clip_width  = MIN(obj_output->output_surface_width, drawable_width);
    clip_height = MIN(obj_output->output_surface_height, drawable_height);
    vdp_status  = vdpau_presentation_queue_display(driver_data,
                                                   obj_output->vdp_flip_queue,
                                                   vdp_output_surface,
                                                   clip_width,
                                                   clip_height,
                                                   0);

    if (vdp_status != VDP_STATUS_OK)
        return vdpau_translate_VAStatus(driver_data, vdp_status);

    obj_surface->va_surface_status     = VASurfaceDisplaying;
    obj_surface->vdp_output_surface    = vdp_output_surface;
    obj_output->current_output_surface = (obj_output->current_output_surface + 1) % VDPAU_MAX_OUTPUT_SURFACES;
    return VA_STATUS_SUCCESS;
}

static VAStatus
vdpau_PutSurface(VADriverContextP ctx,
                 VASurfaceID surface,
                 Drawable draw,                 /* X Drawable */
                 short srcx,
                 short srcy,
                 unsigned short srcw,
                 unsigned short srch,
                 short destx,
                 short desty,
                 unsigned short destw,
                 unsigned short desth,
                 VARectangle *cliprects,        /* client supplied clip list */
                 unsigned int number_cliprects, /* number of clip rects in the clip list */
                 unsigned int flags)            /* de-interlacing flags */
{
    INIT_DRIVER_DATA;

    /* XXX: no clip rects supported */
    if (cliprects || number_cliprects > 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    /* XXX: only support VA_FRAME_PICTURE */
    if (flags)
        return VA_STATUS_ERROR_FLAG_NOT_SUPPORTED;

    unsigned int w, h;
    get_drawable_size(ctx->x11_dpy, draw, &w, &h);

    VdpRect src_rect, dst_rect;
    src_rect.x0 = srcx;
    src_rect.y0 = srcy;
    src_rect.x1 = src_rect.x0 + srcw;
    src_rect.y1 = src_rect.y0 + srch;
    dst_rect.x0 = destx;
    dst_rect.y0 = desty;
    dst_rect.x1 = dst_rect.x0 + destw;
    dst_rect.y1 = dst_rect.y0 + desth;
    return put_surface(driver_data, surface, draw, w, h, &src_rect, &dst_rect);
}

#if USE_GLX
// Ensure GLX TFP and FBO extensions are available
static int
glx_ensure_extensions(vdpau_driver_data_t *driver_data)
{
    opengl_data_t * const gl_data = &driver_data->gl_data;

    if (gl_data->gl_status == OPENGL_STATUS_NONE) {
        gl_data->gl_status = OPENGL_STATUS_ERROR;
        if (gl_check_extensions(driver_data) < 0)
            return -1;
        if (gl_load_funcs(driver_data) < 0)
            return -1;
        gl_data->gl_status = OPENGL_STATUS_OK;
    }

    if (gl_data->gl_status != OPENGL_STATUS_OK)
        return -1;
    return 0;
}

// Ensure X11 and GLX pixmaps are on par with texture requirements
static int
glx_ensure_pixmaps(vdpau_driver_data_t *driver_data,
                   object_output_p      obj_output,
                   GLuint               texture)
{
    VADriverContextP const ctx = driver_data->va_context;

    unsigned int internal_format, border_width, width, height;
    if (gl_get_texture_param(GL_TEXTURE_INTERNAL_FORMAT, &internal_format) < 0)
        return -1;
    if (internal_format != GL_RGBA)
        return -1;
    if (gl_get_texture_param(GL_TEXTURE_BORDER, &border_width) < 0)
        return -1;
    if (gl_get_texture_param(GL_TEXTURE_WIDTH, &width) < 0)
        return -1;
    if (gl_get_texture_param(GL_TEXTURE_HEIGHT, &height) < 0)
        return -1;
    width  -= 2 * border_width;
    height -= 2 * border_width;
    if (width == 0 || height == 0)
        return -1;

    /* Check wether we have to re-create the pixmaps */
    int width_changed  = obj_output->width  != width;
    int height_changed = obj_output->height != height;

    if (width_changed || height_changed) {
        if (obj_output->glx_pixmap) {
            glXDestroyPixmap(ctx->x11_dpy, obj_output->glx_pixmap);
            obj_output->glx_pixmap = None;
        }
        if (obj_output->pixmap) {
            XFreePixmap(ctx->x11_dpy, obj_output->pixmap);
            obj_output->pixmap = None;
        }
        obj_output->width  = width;
        obj_output->height = height;
    }

    /* Create X11 Pixmap */
    if (obj_output->pixmap == None || width_changed || height_changed) {
        Pixmap pixmap = gen_pixmap(driver_data, width, height);
        if (pixmap == None)
            return -1;
        obj_output->pixmap = pixmap;
    }
    ASSERT(obj_output->pixmap);

    /* Create GLX Pixmap */
    if (obj_output->glx_pixmap == None) {
        GLXPixmap glx_pixmap = gen_glx_pixmap(driver_data, obj_output->pixmap);
        if (glx_pixmap == None)
            return -1;
        obj_output->glx_pixmap = glx_pixmap;
    }
    ASSERT(obj_output->glx_pixmap);
    return 0;
}

// Bind GLX Pixmap to texture
static int
glx_bind_pixmap(vdpau_driver_data_t *driver_data, object_output_p obj_output)
{
    VADriverContextP const ctx = driver_data->va_context;
    opengl_data_t * const gl_data = &driver_data->gl_data;

    if (obj_output->is_bound)
        return 0;

    x11_trap_errors();
    gl_data->glx_bind_tex_image(ctx->x11_dpy, obj_output->glx_pixmap,
                                GLX_FRONT_LEFT_EXT, NULL);
    XSync(ctx->x11_dpy, False);
    if (x11_untrap_errors() != 0) {
        vdpau_error_message("failed to bind pixmap\n");
        return -1;
    }

    obj_output->is_bound = 1;
    return 0;
}

// Release GLX Pixmap from texture
static int
glx_release_pixmap(vdpau_driver_data_t *driver_data, object_output_p obj_output)
{
    VADriverContextP const ctx = driver_data->va_context;
    opengl_data_t * const gl_data = &driver_data->gl_data;

    if (!obj_output->is_bound)
        return 0;

    x11_trap_errors();
    gl_data->glx_release_tex_image(ctx->x11_dpy, obj_output->glx_pixmap,
                                   GLX_FRONT_LEFT_EXT);
    XSync(ctx->x11_dpy, False);
    if (x11_untrap_errors() != 0) {
        vdpau_error_message("failed to release pixmap\n");
        return -1;
    }

    obj_output->is_bound = 0;
    return 0;
}

// Render GLX Pixmap to texture
static int
glx_render_pixmap(vdpau_driver_data_t *driver_data, object_output_p obj_output)
{
    const unsigned int w = obj_output->width;
    const unsigned int h = obj_output->height;
    float old_color[4];

    glBindTexture(GL_TEXTURE_2D, obj_output->fbo_texture);
    if (glx_bind_pixmap(driver_data, obj_output) < 0)
        return -1;
    gl_get_current_color(old_color);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glBegin(GL_QUADS);
    {
        glTexCoord2f(0.0f, 0.0f); glVertex2i(0, 0);
        glTexCoord2f(0.0f, 1.0f); glVertex2i(0, h);
        glTexCoord2f(1.0f, 1.0f); glVertex2i(w, h);
        glTexCoord2f(1.0f, 0.0f); glVertex2i(w, 0);
    }
    glEnd();
    glColor4fv(old_color);
    if (glx_release_pixmap(driver_data, obj_output) < 0)
        return -1;
    return 0;
}

// Setup matrices to match the FBO texture dimensions
static int
glx_fbo_enter(vdpau_driver_data_t *driver_data, object_output_p obj_output)
{
    opengl_data_t * const gl_data = &driver_data->gl_data;
    const unsigned int width  = obj_output->width;
    const unsigned int height = obj_output->height;

    gl_data->gl_bind_framebuffer(GL_FRAMEBUFFER_EXT, obj_output->fbo);
    glPushAttrib(GL_VIEWPORT_BIT);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glViewport(0, 0, width, height);
    glTranslatef(-1.0f, -1.0f, 0.0f);
    glScalef(2.0f / width, 2.0f / height, 1.0f);
    return 0;
}

// Restore original OpenGL matrices
static int
glx_fbo_leave(vdpau_driver_data_t *driver_data)
{
    opengl_data_t * const gl_data = &driver_data->gl_data;

    glPopAttrib();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    gl_data->gl_bind_framebuffer(GL_FRAMEBUFFER_EXT, 0);
    return 0;
}

// vaCopySurfaceToTextureGLX
static VAStatus
vdpau_CopySurfaceToTextureGLX(VADriverContextP  ctx,
                              VASurfaceID       surface,
                              unsigned int      texture,
                              unsigned int      flags)
{
    INIT_DRIVER_DATA;

    object_surface_p obj_surface = SURFACE(surface);
    ASSERT(obj_surface);
    if (obj_surface == NULL)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    object_context_p obj_context = CONTEXT(obj_surface->va_context);
    ASSERT(obj_context);
    if (obj_context == NULL)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    object_output_p obj_output = OUTPUT(obj_context->output_surface);
    ASSERT(obj_output);
    if (obj_output == NULL)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    /* XXX: only support VA_FRAME_PICTURE */
    if (flags)
        return VA_STATUS_ERROR_FLAG_NOT_SUPPORTED;

    /* Make sure we have the necessary GLX extensions */
    if (glx_ensure_extensions(driver_data) < 0)
        return VA_STATUS_ERROR_OPERATION_FAILED;

    /* Make sure it is a valid GL texture object */
    if (!glIsTexture(texture))
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    /* Enable 2D texture */
    int was_texture_2d = glIsEnabled(GL_TEXTURE_2D);
    if (!was_texture_2d)
        glEnable(GL_TEXTURE_2D);

    /* Make sure binding succeeds, if texture was not already bound */
    GLuint old_texture = 0;
    if (was_texture_2d && gl_get_param(GL_TEXTURE_BINDING_2D, &old_texture) < 0)
        return VA_STATUS_ERROR_OPERATION_FAILED;
    if (texture != old_texture) {
        gl_purge_errors();
        glBindTexture(GL_TEXTURE_2D, texture);
        if (gl_check_error())
            return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    /* Ensure Pixmaps are on par with the underlying texture */
    if (glx_ensure_pixmaps(driver_data, obj_output, texture) < 0)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    unsigned int width  = obj_output->width;
    unsigned int height = obj_output->height;

    /* Create FBO data */
    if (obj_output->fbo == 0 ||
        obj_output->fbo_buffer == 0 ||
        obj_output->fbo_texture == 0) {
        GLuint fbo, fbo_buffer, fbo_texture;
        if (gen_fbo_data(driver_data,
                         texture, width, height,
                         &fbo, &fbo_buffer, &fbo_texture) < 0)
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        obj_output->fbo         = fbo;
        obj_output->fbo_buffer  = fbo_buffer;
        obj_output->fbo_texture = fbo_texture;
    }
    ASSERT(obj_output->fbo > 0);
    ASSERT(obj_output->fbo_buffer > 0);
    ASSERT(obj_output->fbo_texture > 0);

    /* Render to Pixmap */
    VAStatus va_status;
    VdpRect src_rect, dst_rect;
    src_rect.x0 = 0;
    src_rect.y0 = 0;
    src_rect.x1 = obj_surface->width;
    src_rect.y1 = obj_surface->height;
    dst_rect.x0 = 0;
    dst_rect.y0 = 0;
    dst_rect.x1 = width;
    dst_rect.y1 = height;
    va_status = put_surface(driver_data, surface,
                            obj_output->pixmap, width, height,
                            &src_rect, &dst_rect);
    if (va_status != VA_STATUS_SUCCESS)
        return va_status;
    va_status = sync_surface(driver_data, obj_context, obj_surface);
    if (va_status != VA_STATUS_SUCCESS)
        return va_status;

    /* Render to FBO */
    glx_fbo_enter(driver_data, obj_output);
    glx_render_pixmap(driver_data, obj_output);
    glx_fbo_leave(driver_data);

    /* Restore previous state */
    if (was_texture_2d)
        glBindTexture(GL_TEXTURE_2D, old_texture);
    else
        glDisable(GL_TEXTURE_2D);
    return VA_STATUS_SUCCESS;
}

// vaBindSurfaceToTextureGLX
static VAStatus
vdpau_BindSurfaceToTextureGLX(VADriverContextP ctx,
                              VASurfaceID surface,
                              unsigned int texture,
                              unsigned int flags)
{
    INIT_DRIVER_DATA;

    object_surface_p obj_surface = SURFACE(surface);
    ASSERT(obj_surface);
    if (obj_surface == NULL)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    object_context_p obj_context = CONTEXT(obj_surface->va_context);
    ASSERT(obj_context);
    if (obj_context == NULL)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    object_output_p obj_output = OUTPUT(obj_context->output_surface);
    ASSERT(obj_output);
    if (obj_output == NULL)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    /* XXX: only support VA_FRAME_PICTURE */
    if (flags)
        return VA_STATUS_ERROR_FLAG_NOT_SUPPORTED;

    /* Make sure we have the necessary GLX extensions */
    if (glx_ensure_extensions(driver_data) < 0)
        return VA_STATUS_ERROR_OPERATION_FAILED;

    /* Make sure it is a valid GL texture object */
    if (!glIsTexture(texture))
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    /* Make sure binding succeeds */
    gl_purge_errors();
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, texture);
    if (gl_check_error())
        return VA_STATUS_ERROR_OPERATION_FAILED;

    /* Ensure Pixmaps are on par with the underlying texture */
    if (glx_ensure_pixmaps(driver_data, obj_output, texture) < 0)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    unsigned int width  = obj_output->width;
    unsigned int height = obj_output->height;

    /* Render to Pixmap */
    VAStatus va_status;
    VdpRect src_rect, dst_rect;
    src_rect.x0 = 0;
    src_rect.y0 = 0;
    src_rect.x1 = obj_surface->width;
    src_rect.y1 = obj_surface->height;
    dst_rect.x0 = 0;
    dst_rect.y0 = 0;
    dst_rect.x1 = width;
    dst_rect.y1 = height;
    va_status = put_surface(driver_data, surface,
                            obj_output->pixmap, width, height,
                            &src_rect, &dst_rect);
    if (va_status != VA_STATUS_SUCCESS)
        return va_status;
    va_status = sync_surface(driver_data, obj_context, obj_surface);
    if (va_status != VA_STATUS_SUCCESS)
        return va_status;

    /* Bind GLX Pixmap */
    if (glx_bind_pixmap(driver_data, obj_output) < 0)
        return VA_STATUS_ERROR_OPERATION_FAILED;

    return VA_STATUS_SUCCESS;
}

// vaReleaseSurfaceFromTextureGLX
static VAStatus
vdpau_ReleaseSurfaceFromTextureGLX(VADriverContextP ctx, VASurfaceID surface)
{
    INIT_DRIVER_DATA;

    object_surface_p obj_surface = SURFACE(surface);
    ASSERT(obj_surface);
    if (obj_surface == NULL)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    object_context_p obj_context = CONTEXT(obj_surface->va_context);
    ASSERT(obj_context);
    if (obj_context == NULL)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    object_output_p obj_output = OUTPUT(obj_context->output_surface);
    ASSERT(obj_output);
    if (obj_output == NULL)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    /* Unbind GLX Pixmap */
    if (glx_release_pixmap(driver_data, obj_output) < 0)
        return VA_STATUS_ERROR_OPERATION_FAILED;

    glBindTexture(GL_TEXTURE_2D, 0);
    return VA_STATUS_SUCCESS;
}
#endif

// vaQueryDisplayAttributes
static VAStatus
vdpau_QueryDisplayAttributes(VADriverContextP ctx,
                             VADisplayAttribute *attr_list,     /* out */
                             int *num_attributes)               /* out */
{
    /* TODO */
    return VA_STATUS_ERROR_UNKNOWN;
}

// vaGetDisplayAttributes
static VAStatus
vdpau_GetDisplayAttributes(VADriverContextP ctx,
                           VADisplayAttribute *attr_list,       /* in/out */
                           int num_attributes)
{
    /* TODO */
    return VA_STATUS_ERROR_UNKNOWN;
}

// vaSetDisplayAttributes
static VAStatus
vdpau_SetDisplayAttributes(VADriverContextP ctx,
                           VADisplayAttribute *attr_list,
                           int num_attributes)
{
    /* TODO */
    return VA_STATUS_ERROR_UNKNOWN;
}

// vaDbgCopySurfaceToBuffer (not a PUBLIC interface)
static VAStatus
vdpau_DbgCopySurfaceToBuffer(VADriverContextP ctx,
                             VASurfaceID surface,
                             void **buffer,             /* out */
                             unsigned int *stride)      /* out */
{
    /* TODO */
    return VA_STATUS_ERROR_UNKNOWN;
}

#if VA_CHECK_VERSION(0,30,0)
// vaCreateSurfaceFromCIFrame
static VAStatus
vdpau_CreateSurfaceFromCIFrame(VADriverContextP ctx,
                               unsigned long frame_id,
                               VASurfaceID *surface)    /* out */
{
    /* TODO */
    return VA_STATUS_ERROR_UNKNOWN;
}

// vaCreateSurfaceFromV4L2Buf
static VAStatus
vdpau_CreateSurfaceFromV4L2Buf(VADriverContextP ctx,
                               int v4l2_fd,                     /* file descriptor of V4L2 device */
                               struct v4l2_format *v4l2_fmt,    /* format of V4L2 */
                               struct v4l2_buffer *v4l2_buf,    /* V4L2 buffer */
                               VASurfaceID *surface)            /* out */
{
    /* TODO */
    return VA_STATUS_ERROR_UNKNOWN;
}

// vaCopySurfaceToBuffer
static VAStatus
vdpau_CopySurfaceToBuffer(VADriverContextP ctx,
                          VASurfaceID surface,
                          unsigned int *fourcc, /* out  for follow argument */
                          unsigned int *luma_stride,
                          unsigned int *chroma_u_stride,
                          unsigned int *chroma_v_stride,
                          unsigned int *luma_offset,
                          unsigned int *chroma_u_offset,
                          unsigned int *chroma_v_offset,
                          void **buffer)
{
    /* TODO */
    return VA_STATUS_ERROR_UNKNOWN;
}
#endif

// vaTerminate
static VAStatus
vdpau_Terminate(VADriverContextP ctx)
{
    INIT_DRIVER_DATA;
    object_buffer_p obj_buffer;
    object_output_p obj_output;
    object_config_p obj_config;
    object_heap_iterator iter;

    /* TODO cleanup */
    object_heap_destroy(&driver_data->image_heap);

    /* Clean up left over buffers */
    obj_buffer = (object_buffer_p)object_heap_first(&driver_data->buffer_heap, &iter);
    while (obj_buffer) {
        vdpau_information_message("vaTerminate: buffer 0x%08x is still allocated, destroying\n", obj_buffer->base.id);
        vdpau_destroy_buffer(driver_data, obj_buffer);
        obj_buffer = (object_buffer_p)object_heap_next(&driver_data->buffer_heap, &iter);
    }
    object_heap_destroy(&driver_data->buffer_heap);

    obj_output = (object_output_p)object_heap_first(&driver_data->output_heap, &iter);
    while (obj_output) {
        vdpau_information_message("vaTerminate: output surface 0x%08x is still allocated, destroying\n", obj_output->base.id);
        destroy_output_surface(driver_data, obj_output->base.id);
        obj_output = (object_output_p)object_heap_next(&driver_data->output_heap, &iter);
    }
    object_heap_destroy(&driver_data->output_heap);

    /* TODO cleanup */
    object_heap_destroy(&driver_data->image_heap);

    /* TODO cleanup */
    object_heap_destroy(&driver_data->surface_heap);

    /* TODO cleanup */
    object_heap_destroy(&driver_data->context_heap);

    /* Clean up configIDs */
    obj_config = (object_config_p)object_heap_first(&driver_data->config_heap, &iter);
    while (obj_config)
    {
        object_heap_free(&driver_data->config_heap, (object_base_p)obj_config);
        obj_config = (object_config_p)object_heap_next(&driver_data->config_heap, &iter);
    }
    object_heap_destroy(&driver_data->config_heap);

    free(ctx->pDriverData);
    ctx->pDriverData = NULL;

    return VA_STATUS_SUCCESS;
}

#define VDPAU_TERMINATE(CTX, STATUS) \
        vdpau_Terminate_with_status(CTX, VA_STATUS_##STATUS)

static inline VAStatus vdpau_Terminate_with_status(VADriverContextP ctx, VAStatus status)
{
    vdpau_Terminate(ctx);
    return status;
}

// vaInitialize
static VAStatus
vdpau_Initialize(VADriverContextP ctx)
{
    struct vdpau_driver_data *driver_data;
    static char vendor[256] = {0, };
    int result;
    VdpStatus vdp_status;

    if (vendor[0] == '\0')
        sprintf(vendor, "%s %s - %d.%d.%d",
                VDPAU_STR_DRIVER_VENDOR,
                VDPAU_STR_DRIVER_NAME,
                VDPAU_VIDEO_MAJOR_VERSION,
                VDPAU_VIDEO_MINOR_VERSION,
                VDPAU_VIDEO_MICRO_VERSION);

    ctx->version_major          = VA_MAJOR_VERSION;
    ctx->version_minor          = VA_MINOR_VERSION;
    ctx->max_profiles           = VDPAU_MAX_PROFILES;
    ctx->max_entrypoints        = VDPAU_MAX_ENTRYPOINTS;
    ctx->max_attributes         = VDPAU_MAX_CONFIG_ATTRIBUTES;
    ctx->max_image_formats      = VDPAU_MAX_IMAGE_FORMATS;
    ctx->max_subpic_formats     = VDPAU_MAX_SUBPIC_FORMATS;
    ctx->max_display_attributes = VDPAU_MAX_DISPLAY_ATTRIBUTES;
    ctx->str_vendor             = vendor;

    ctx->vtable.vaTerminate                     = vdpau_Terminate;
    ctx->vtable.vaQueryConfigEntrypoints        = vdpau_QueryConfigEntrypoints;
    ctx->vtable.vaQueryConfigProfiles           = vdpau_QueryConfigProfiles;
    ctx->vtable.vaQueryConfigEntrypoints        = vdpau_QueryConfigEntrypoints;
    ctx->vtable.vaQueryConfigAttributes         = vdpau_QueryConfigAttributes;
    ctx->vtable.vaCreateConfig                  = vdpau_CreateConfig;
    ctx->vtable.vaDestroyConfig                 = vdpau_DestroyConfig;
    ctx->vtable.vaGetConfigAttributes           = vdpau_GetConfigAttributes;
    ctx->vtable.vaCreateSurfaces                = vdpau_CreateSurfaces;
    ctx->vtable.vaDestroySurfaces               = vdpau_DestroySurfaces;
    ctx->vtable.vaCreateContext                 = vdpau_CreateContext;
    ctx->vtable.vaDestroyContext                = vdpau_DestroyContext;
    ctx->vtable.vaCreateBuffer                  = vdpau_CreateBuffer;
    ctx->vtable.vaBufferSetNumElements          = vdpau_BufferSetNumElements;
    ctx->vtable.vaMapBuffer                     = vdpau_MapBuffer;
    ctx->vtable.vaUnmapBuffer                   = vdpau_UnmapBuffer;
    ctx->vtable.vaDestroyBuffer                 = vdpau_DestroyBuffer;
    ctx->vtable.vaBeginPicture                  = vdpau_BeginPicture;
    ctx->vtable.vaRenderPicture                 = vdpau_RenderPicture;
    ctx->vtable.vaEndPicture                    = vdpau_EndPicture;
    ctx->vtable.vaSyncSurface                   = vdpau_SyncSurface;
    ctx->vtable.vaQuerySurfaceStatus            = vdpau_QuerySurfaceStatus;
    ctx->vtable.vaPutSurface                    = vdpau_PutSurface;
    ctx->vtable.vaQueryImageFormats             = vdpau_QueryImageFormats;
    ctx->vtable.vaCreateImage                   = vdpau_CreateImage;
    ctx->vtable.vaDeriveImage                   = vdpau_DeriveImage;
    ctx->vtable.vaDestroyImage                  = vdpau_DestroyImage;
    ctx->vtable.vaSetImagePalette               = vdpau_SetImagePalette;
    ctx->vtable.vaGetImage                      = vdpau_GetImage;
    ctx->vtable.vaPutImage                      = vdpau_PutImage;
    ctx->vtable.vaPutImage2                     = vdpau_PutImage2;
    ctx->vtable.vaQuerySubpictureFormats        = vdpau_QuerySubpictureFormats;
    ctx->vtable.vaCreateSubpicture              = vdpau_CreateSubpicture;
    ctx->vtable.vaDestroySubpicture             = vdpau_DestroySubpicture;
    ctx->vtable.vaSetSubpictureImage            = vdpau_SetSubpictureImage;
    ctx->vtable.vaSetSubpictureChromakey        = vdpau_SetSubpictureChromakey;
    ctx->vtable.vaSetSubpictureGlobalAlpha      = vdpau_SetSubpictureGlobalAlpha;
    ctx->vtable.vaAssociateSubpicture           = vdpau_AssociateSubpicture;
    ctx->vtable.vaAssociateSubpicture2          = vdpau_AssociateSubpicture2;
    ctx->vtable.vaDeassociateSubpicture         = vdpau_DeassociateSubpicture;
    ctx->vtable.vaQueryDisplayAttributes        = vdpau_QueryDisplayAttributes;
    ctx->vtable.vaGetDisplayAttributes          = vdpau_GetDisplayAttributes;
    ctx->vtable.vaSetDisplayAttributes          = vdpau_SetDisplayAttributes;
#if VA_CHECK_VERSION(0,30,0)
    ctx->vtable.vaCreateSurfaceFromCIFrame      = vdpau_CreateSurfaceFromCIFrame;
    ctx->vtable.vaCreateSurfaceFromV4L2Buf      = vdpau_CreateSurfaceFromV4L2Buf;
    ctx->vtable.vaCopySurfaceToBuffer           = vdpau_CopySurfaceToBuffer;
#else
    ctx->vtable.vaSetSubpicturePalette          = vdpau_SetSubpicturePalette;
    ctx->vtable.vaDbgCopySurfaceToBuffer        = vdpau_DbgCopySurfaceToBuffer;
#endif
#if USE_GLX
    ctx->vtable.vaCopySurfaceToTextureGLX       = vdpau_CopySurfaceToTextureGLX;
    ctx->vtable.vaBindSurfaceToTextureGLX       = vdpau_BindSurfaceToTextureGLX;
    ctx->vtable.vaReleaseSurfaceFromTextureGLX  = vdpau_ReleaseSurfaceFromTextureGLX;
#endif

    driver_data = (struct vdpau_driver_data *)calloc(1, sizeof(*driver_data));
    if (driver_data == NULL)
        return VDPAU_TERMINATE(ctx, ERROR_ALLOCATION_FAILED);
    ctx->pDriverData = (void *)driver_data;
    driver_data->va_context = ctx;

    vdp_status = vdp_device_create_x11(ctx->x11_dpy, ctx->x11_screen,
                                       &driver_data->vdp_device,
                                       &driver_data->vdp_get_proc_address);
    ASSERT(vdp_status == VDP_STATUS_OK);
    if (vdp_status != VDP_STATUS_OK)
        return VDPAU_TERMINATE(ctx, ERROR_UNKNOWN);

#define VDP_INIT_PROC(FUNC_ID, FUNC) do {                       \
        vdp_status = driver_data->vdp_get_proc_address          \
            (driver_data->vdp_device,                           \
             VDP_FUNC_ID_##FUNC_ID,                             \
             (void *)&driver_data->vdp_vtable.vdp_##FUNC);      \
        ASSERT(vdp_status == VDP_STATUS_OK);                    \
        if (vdp_status != VDP_STATUS_OK)                        \
            return VDPAU_TERMINATE(ctx, ERROR_UNKNOWN);         \
    } while (0)

    VDP_INIT_PROC(DEVICE_DESTROY,               device_destroy);
    VDP_INIT_PROC(VIDEO_SURFACE_CREATE,         video_surface_create);
    VDP_INIT_PROC(VIDEO_SURFACE_DESTROY,        video_surface_destroy);
    VDP_INIT_PROC(VIDEO_SURFACE_GET_BITS_Y_CB_CR,
                  video_surface_get_bits_ycbcr);
    VDP_INIT_PROC(OUTPUT_SURFACE_CREATE,        output_surface_create);
    VDP_INIT_PROC(OUTPUT_SURFACE_DESTROY,       output_surface_destroy);
    VDP_INIT_PROC(OUTPUT_SURFACE_GET_BITS_NATIVE,
                  output_surface_get_bits_native);
    VDP_INIT_PROC(VIDEO_MIXER_CREATE,           video_mixer_create);
    VDP_INIT_PROC(VIDEO_MIXER_DESTROY,          video_mixer_destroy);
    VDP_INIT_PROC(VIDEO_MIXER_RENDER,           video_mixer_render);
    VDP_INIT_PROC(PRESENTATION_QUEUE_CREATE,    presentation_queue_create);
    VDP_INIT_PROC(PRESENTATION_QUEUE_DESTROY,   presentation_queue_destroy);
    VDP_INIT_PROC(PRESENTATION_QUEUE_DISPLAY,   presentation_queue_display);
    VDP_INIT_PROC(PRESENTATION_QUEUE_BLOCK_UNTIL_SURFACE_IDLE,
                  presentation_queue_block_until_surface_idle);
    VDP_INIT_PROC(PRESENTATION_QUEUE_QUERY_SURFACE_STATUS,
                  presentation_queue_query_surface_status);
    VDP_INIT_PROC(PRESENTATION_QUEUE_TARGET_CREATE_X11,
                  presentation_queue_target_create_x11);
    VDP_INIT_PROC(PRESENTATION_QUEUE_TARGET_DESTROY,
                  presentation_queue_target_destroy);
    VDP_INIT_PROC(DECODER_CREATE,               decoder_create);
    VDP_INIT_PROC(DECODER_DESTROY,              decoder_destroy);
    VDP_INIT_PROC(DECODER_RENDER,               decoder_render);
    VDP_INIT_PROC(DECODER_QUERY_CAPABILITIES,   decoder_query_capabilities);
    VDP_INIT_PROC(VIDEO_SURFACE_QUERY_GET_PUT_BITS_Y_CB_CR_CAPABILITIES,
                  video_surface_query_ycbcr_caps);
    VDP_INIT_PROC(OUTPUT_SURFACE_QUERY_GET_PUT_BITS_NATIVE_CAPABILITIES,
                  output_surface_query_rgba_caps);
    VDP_INIT_PROC(GET_API_VERSION,              get_api_version);
    VDP_INIT_PROC(GET_INFORMATION_STRING,       get_information_string);
    VDP_INIT_PROC(GET_ERROR_STRING,             get_error_string);

#undef VDP_INIT_PROC

    uint32_t api_version;
    vdp_status = vdpau_get_api_version(driver_data, &api_version);
    ASSERT(vdp_status == VDP_STATUS_OK);
    if (api_version != VDPAU_VERSION)
        return VA_STATUS_ERROR_UNKNOWN;

    const char *impl_string = NULL;
    vdp_status = vdpau_get_information_string(driver_data, &impl_string);
    ASSERT(vdp_status == VDP_STATUS_OK);
    if (impl_string) {
        /* XXX: set impl_type and impl_version if there is any useful info */
    }

    result = object_heap_init(&driver_data->config_heap, sizeof(struct object_config), CONFIG_ID_OFFSET);
    ASSERT(result == 0);
    if (result != 0)
        return VDPAU_TERMINATE(ctx, ERROR_ALLOCATION_FAILED);

    result = object_heap_init(&driver_data->context_heap, sizeof(struct object_context), CONTEXT_ID_OFFSET);
    ASSERT(result == 0);
    if (result != 0)
        return VDPAU_TERMINATE(ctx, ERROR_ALLOCATION_FAILED);

    result = object_heap_init(&driver_data->surface_heap, sizeof(struct object_surface), SURFACE_ID_OFFSET);
    ASSERT(result == 0);
    if (result != 0)
        return VDPAU_TERMINATE(ctx, ERROR_ALLOCATION_FAILED);

    result = object_heap_init(&driver_data->buffer_heap, sizeof(struct object_buffer), BUFFER_ID_OFFSET);
    ASSERT(result == 0);
    if (result != 0)
        return VDPAU_TERMINATE(ctx, ERROR_ALLOCATION_FAILED);

    result = object_heap_init(&driver_data->output_heap, sizeof(struct object_output), OUTPUT_ID_OFFSET);
    ASSERT(result == 0);
    if (result != 0)
        return VDPAU_TERMINATE(ctx, ERROR_ALLOCATION_FAILED);

    result = object_heap_init(&driver_data->image_heap, sizeof(struct object_image), IMAGE_ID_OFFSET);
    ASSERT(result == 0);
    if (result != 0)
        return VDPAU_TERMINATE(ctx, ERROR_ALLOCATION_FAILED);

    return VA_STATUS_SUCCESS;
}

VAStatus VA_DRIVER_INIT_FUNC(VADriverContextP ctx)
{
    return vdpau_Initialize(ctx);
}
