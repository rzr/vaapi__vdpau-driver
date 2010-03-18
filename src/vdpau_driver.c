/*
 *  vdpau_driver.c - VDPAU driver
 *
 *  vdpau-video (C) 2009-2010 Splitted-Desktop Systems
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

#include "sysdeps.h"
#include "vdpau_driver.h"
#include "vdpau_buffer.h"
#include "vdpau_decode.h"
#include "vdpau_image.h"
#include "vdpau_subpic.h"
#include "vdpau_mixer.h"
#include "vdpau_video.h"
#include "vdpau_video_x11.h"
#if USE_GLX
#include "vdpau_video_glx.h"
#include <va/va_backend_glx.h>
#endif

#define DEBUG 1
#include "debug.h"


// Check for VA/SDS version
#define VA_CHECK_VERSION_SDS(major, minor, micro, sds)                  \
    (VA_CHECK_VERSION(major, minor, (micro)+1) ||                       \
     (VA_CHECK_VERSION(major, minor, micro) && VA_SDS_VERSION >= (sds)))

// Check for VA/GLX changes from libVA API >= 0.31.0-sds2
#if VA_CHECK_VERSION_SDS(0,31,0,6)
#define VA_DRIVER_VTABLE_GLX(ctx) get_vtable_glx(ctx)
typedef struct VADriverVTableGLX *VADriverVTableGLXP;

static VADriverVTableGLXP get_vtable_glx(VADriverContextP ctx)
{
    VADriverVTableGLXP vtable_glx = ctx->vtable.glx;

    if (!vtable_glx) {
        vtable_glx = calloc(1, sizeof(*vtable_glx));
        if (!vtable_glx)
            return NULL;
        ctx->vtable.glx = vtable_glx;
    }
    return vtable_glx;
}
#elif VA_CHECK_VERSION_SDS(0,31,0,2)
#define VA_DRIVER_VTABLE_GLX(ctx) (&(ctx)->vtable.glx)
typedef struct VADriverVTableGLX *VADriverVTableGLXP;
#else
#define VA_DRIVER_VTABLE_GLX(ctx) (&(ctx)->vtable)
typedef struct VADriverVTable    *VADriverVTableGLXP;
#endif

// Return TRUE if underlying VDPAU implementation is NVIDIA
VdpBool
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

// Translate VdpStatus to an appropriate VAStatus
VAStatus
vdpau_get_VAStatus(vdpau_driver_data_t *driver_data, VdpStatus vdp_status)
{
    VAStatus va_status;
    const char *vdp_status_string;

    switch (vdp_status) {
    case VDP_STATUS_OK:
        va_status = VA_STATUS_SUCCESS;
        break;
    case VDP_STATUS_NO_IMPLEMENTATION:
        va_status = VA_STATUS_ERROR_UNIMPLEMENTED;
        break;
    case VDP_STATUS_INVALID_CHROMA_TYPE:
        va_status = VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
        break;
    case VDP_STATUS_INVALID_DECODER_PROFILE:
        va_status = VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
        break;
    case VDP_STATUS_RESOURCES:
        va_status = VA_STATUS_ERROR_ALLOCATION_FAILED;
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

// Destroy BUFFER objects
static void destroy_buffer_cb(object_base_p obj, void *user_data)
{
    object_buffer_p const obj_buffer = (object_buffer_p)obj;
    vdpau_driver_data_t * const driver_data = user_data;

    destroy_va_buffer(driver_data, obj_buffer);
}

// Destroy MIXER objects
static void destroy_mixer_cb(object_base_p obj, void *user_data)
{
    object_mixer_p const obj_mixer = (object_mixer_p)obj;
    vdpau_driver_data_t * const driver_data = user_data;

    video_mixer_destroy(driver_data, obj_mixer);
}

// Destroy object heap
typedef void (*destroy_heap_func_t)(object_base_p obj, void *user_data);

static void
destroy_heap(
    const char         *name,
    object_heap_p       heap,
    destroy_heap_func_t destroy_func,
    void               *user_data
)
{
    object_base_p obj;
    object_heap_iterator iter;

    if (!heap)
        return;

    obj = object_heap_first(heap, &iter);
    while (obj) {
        vdpau_information_message("vaTerminate(): %s ID 0x%08x is still allocated, destroying\n", name, obj->id);
        if (destroy_func)
            destroy_func(obj, user_data);
        else
            object_heap_free(heap, obj);
        obj = object_heap_next(heap, &iter);
    }
    object_heap_destroy(heap);
}

#define DESTROY_HEAP(heap, func) \
        destroy_heap(#heap, &driver_data->heap##_heap, func, driver_data)

#define CREATE_HEAP(type, id) do {                                  \
        int result = object_heap_init(&driver_data->type##_heap,    \
                                      sizeof(struct object_##type), \
                                      VDPAU_##id##_ID_OFFSET);      \
        ASSERT(result == 0);                                        \
        if (result != 0)                                            \
            return VA_STATUS_ERROR_UNKNOWN;                         \
    } while (0)

// vaTerminate
static VAStatus vdpau_Terminate(VADriverContextP ctx)
{
    VDPAU_DRIVER_DATA_INIT;

    DESTROY_HEAP(buffer,      destroy_buffer_cb);
    DESTROY_HEAP(image,       NULL);
    DESTROY_HEAP(subpicture,  NULL);
    DESTROY_HEAP(output,      NULL);
    DESTROY_HEAP(surface,     NULL);
    DESTROY_HEAP(context,     NULL);
    DESTROY_HEAP(config,      NULL);
    DESTROY_HEAP(mixer,       destroy_mixer_cb);
#if USE_GLX
    DESTROY_HEAP(glx_surface, NULL);

#if VA_CHECK_VERSION_SDS(0,31,0,6)
    free(ctx->vtable.glx);
    ctx->vtable.glx = NULL;
#endif
#endif

    if (driver_data->vdp_device != VDP_INVALID_HANDLE) {
        vdpau_device_destroy(driver_data, driver_data->vdp_device);
        driver_data->vdp_device = VDP_INVALID_HANDLE;
    }

    free(driver_data->gl_data);
    vdpau_gate_exit(driver_data);

    free(ctx->pDriverData);
    ctx->pDriverData = NULL;

    return VA_STATUS_SUCCESS;
}

// vaInitialize
static VAStatus vdpau_do_Initialize(VADriverContextP ctx)
{
    VDPAU_DRIVER_DATA_INIT;

    VdpStatus vdp_status;
    driver_data->vdp_device = VDP_INVALID_HANDLE;
    vdp_status = vdp_device_create_x11(
        ctx->x11_dpy,
        ctx->x11_screen,
        &driver_data->vdp_device,
        &driver_data->vdp_get_proc_address
    );
    ASSERT(vdp_status == VDP_STATUS_OK);
    if (vdp_status != VDP_STATUS_OK)
        return VA_STATUS_ERROR_UNKNOWN;

    if (vdpau_gate_init(driver_data) < 0)
        return VA_STATUS_ERROR_UNKNOWN;

    uint32_t api_version;
    vdp_status = vdpau_get_api_version(driver_data, &api_version);
    ASSERT(vdp_status == VDP_STATUS_OK);
    if (vdp_status != VDP_STATUS_OK)
        return vdpau_get_VAStatus(driver_data, vdp_status);
    if (api_version != VDPAU_VERSION)
        return VA_STATUS_ERROR_UNKNOWN;

    const char *impl_string = NULL;
    vdp_status = vdpau_get_information_string(driver_data, &impl_string);
    ASSERT(vdp_status == VDP_STATUS_OK);
    if (vdp_status != VDP_STATUS_OK)
        return vdpau_get_VAStatus(driver_data, vdp_status);
    if (impl_string) {
        /* XXX: set impl_type and impl_version if there is any useful info */
    }

    CREATE_HEAP(config, CONFIG);
    CREATE_HEAP(context, CONTEXT);
    CREATE_HEAP(surface, SURFACE);
    CREATE_HEAP(buffer, BUFFER);
    CREATE_HEAP(output, OUTPUT);
    CREATE_HEAP(image, IMAGE);
    CREATE_HEAP(subpicture, SUBPICTURE);
    CREATE_HEAP(mixer, MIXER);
#if USE_GLX
    CREATE_HEAP(glx_surface, GLX_SURFACE);
#endif

    static char vendor[256] = {0, };
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
    ctx->max_subpic_formats     = VDPAU_MAX_SUBPICTURE_FORMATS;
    ctx->max_display_attributes = VDPAU_MAX_DISPLAY_ATTRIBUTES;
    ctx->str_vendor             = vendor;

    ctx->vtable.vaTerminate                 = vdpau_Terminate;
    ctx->vtable.vaQueryConfigEntrypoints    = vdpau_QueryConfigEntrypoints;
    ctx->vtable.vaQueryConfigProfiles       = vdpau_QueryConfigProfiles;
    ctx->vtable.vaQueryConfigEntrypoints    = vdpau_QueryConfigEntrypoints;
    ctx->vtable.vaQueryConfigAttributes     = vdpau_QueryConfigAttributes;
    ctx->vtable.vaCreateConfig              = vdpau_CreateConfig;
    ctx->vtable.vaDestroyConfig             = vdpau_DestroyConfig;
    ctx->vtable.vaGetConfigAttributes       = vdpau_GetConfigAttributes;
    ctx->vtable.vaCreateSurfaces            = vdpau_CreateSurfaces;
    ctx->vtable.vaDestroySurfaces           = vdpau_DestroySurfaces;
    ctx->vtable.vaCreateContext             = vdpau_CreateContext;
    ctx->vtable.vaDestroyContext            = vdpau_DestroyContext;
    ctx->vtable.vaCreateBuffer              = vdpau_CreateBuffer;
    ctx->vtable.vaBufferSetNumElements      = vdpau_BufferSetNumElements;
    ctx->vtable.vaMapBuffer                 = vdpau_MapBuffer;
    ctx->vtable.vaUnmapBuffer               = vdpau_UnmapBuffer;
    ctx->vtable.vaDestroyBuffer             = vdpau_DestroyBuffer;
    ctx->vtable.vaBeginPicture              = vdpau_BeginPicture;
    ctx->vtable.vaRenderPicture             = vdpau_RenderPicture;
    ctx->vtable.vaEndPicture                = vdpau_EndPicture;
#if VA_CHECK_VERSION(0,31,0)
    ctx->vtable.vaSyncSurface               = vdpau_SyncSurface2;
#else
    ctx->vtable.vaSyncSurface               = vdpau_SyncSurface3;
#endif
    ctx->vtable.vaQuerySurfaceStatus        = vdpau_QuerySurfaceStatus;
    ctx->vtable.vaPutSurface                = vdpau_PutSurface;
    ctx->vtable.vaQueryImageFormats         = vdpau_QueryImageFormats;
    ctx->vtable.vaCreateImage               = vdpau_CreateImage;
    ctx->vtable.vaDeriveImage               = vdpau_DeriveImage;
    ctx->vtable.vaDestroyImage              = vdpau_DestroyImage;
    ctx->vtable.vaSetImagePalette           = vdpau_SetImagePalette;
    ctx->vtable.vaGetImage                  = vdpau_GetImage;
#if VA_CHECK_VERSION(0,31,0)
    ctx->vtable.vaPutImage                  = vdpau_PutImage_full;
#else
    ctx->vtable.vaPutImage                  = vdpau_PutImage;
    ctx->vtable.vaPutImage2                 = vdpau_PutImage_full;
#endif
    ctx->vtable.vaQuerySubpictureFormats    = vdpau_QuerySubpictureFormats;
    ctx->vtable.vaCreateSubpicture          = vdpau_CreateSubpicture;
    ctx->vtable.vaDestroySubpicture         = vdpau_DestroySubpicture;
    ctx->vtable.vaSetSubpictureImage        = vdpau_SetSubpictureImage;
    ctx->vtable.vaSetSubpictureChromakey    = vdpau_SetSubpictureChromakey;
    ctx->vtable.vaSetSubpictureGlobalAlpha  = vdpau_SetSubpictureGlobalAlpha;
#if VA_CHECK_VERSION(0,31,0)
    ctx->vtable.vaAssociateSubpicture       = vdpau_AssociateSubpicture_full;
#else
    ctx->vtable.vaAssociateSubpicture       = vdpau_AssociateSubpicture;
    ctx->vtable.vaAssociateSubpicture2      = vdpau_AssociateSubpicture_full;
#endif
    ctx->vtable.vaDeassociateSubpicture     = vdpau_DeassociateSubpicture;
    ctx->vtable.vaQueryDisplayAttributes    = vdpau_QueryDisplayAttributes;
    ctx->vtable.vaGetDisplayAttributes      = vdpau_GetDisplayAttributes;
    ctx->vtable.vaSetDisplayAttributes      = vdpau_SetDisplayAttributes;
#if VA_CHECK_VERSION(0,30,0)
    ctx->vtable.vaCreateSurfaceFromCIFrame  = vdpau_CreateSurfaceFromCIFrame;
    ctx->vtable.vaCreateSurfaceFromV4L2Buf  = vdpau_CreateSurfaceFromV4L2Buf;
    ctx->vtable.vaCopySurfaceToBuffer       = vdpau_CopySurfaceToBuffer;
#else
    ctx->vtable.vaSetSubpicturePalette      = vdpau_SetSubpicturePalette;
    ctx->vtable.vaDbgCopySurfaceToBuffer    = vdpau_DbgCopySurfaceToBuffer;
#endif

#if USE_GLX
    VADriverVTableGLXP const glx_vtable     = VA_DRIVER_VTABLE_GLX(ctx);
    if (!glx_vtable)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    glx_vtable->vaCreateSurfaceGLX          = vdpau_CreateSurfaceGLX;
    glx_vtable->vaDestroySurfaceGLX         = vdpau_DestroySurfaceGLX;
#if !VA_CHECK_VERSION_SDS(0,31,0,5) /* 0.31.0-sds5 dropped 'bind' API */
    glx_vtable->vaAssociateSurfaceGLX       = vdpau_AssociateSurfaceGLX;
    glx_vtable->vaDeassociateSurfaceGLX     = vdpau_DeassociateSurfaceGLX;
    glx_vtable->vaSyncSurfaceGLX            = vdpau_SyncSurfaceGLX;
    glx_vtable->vaBeginRenderSurfaceGLX     = vdpau_BeginRenderSurfaceGLX;
    glx_vtable->vaEndRenderSurfaceGLX       = vdpau_EndRenderSurfaceGLX;
#endif
    glx_vtable->vaCopySurfaceGLX            = vdpau_CopySurfaceGLX;
#endif

    return VA_STATUS_SUCCESS;
}

static VAStatus vdpau_Initialize(VADriverContextP ctx)
{
    struct vdpau_driver_data *driver_data;

    driver_data = (struct vdpau_driver_data *)calloc(1, sizeof(*driver_data));
    if (!driver_data)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    ctx->pDriverData = (void *)driver_data;
    driver_data->va_context = ctx;

    VAStatus va_status = vdpau_do_Initialize(ctx);
    if (va_status != VA_STATUS_SUCCESS)
        vdpau_Terminate(ctx);
    return va_status;
}

VAStatus VA_DRIVER_INIT_FUNC(VADriverContextP ctx)
{
    return vdpau_Initialize(ctx);
}
