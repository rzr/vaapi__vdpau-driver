/*
 *  vdpau_image.c - VDPAU backend for VA API (VA images)
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

#include "sysdeps.h"
#include "vdpau_image.h"
#include "vdpau_video.h"
#include "vdpau_buffer.h"

#define DEBUG 1
#include "debug.h"


// List of supported image formats
typedef enum {
    VDP_IMAGE_FORMAT_TYPE_YCBCR = 1,
    VDP_IMAGE_FORMAT_TYPE_RGBA,
    VDP_IMAGE_FORMAT_TYPE_INDEXED
} VdpImageFormatType;

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

// Checks whether the VDPAU implementation supports the specified image format
static inline VdpBool
is_supported_format(
    vdpau_driver_data_t *driver_data,
    VdpImageFormatType   type,
    uint32_t             format
)
{
    VdpBool is_supported = VDP_FALSE;
    VdpStatus vdp_status;

    switch (type) {
    case VDP_IMAGE_FORMAT_TYPE_YCBCR:
        vdp_status =
            vdpau_video_surface_query_ycbcr_caps(driver_data,
                                                 driver_data->vdp_device,
                                                 VDP_CHROMA_TYPE_420,
                                                 format,
                                                 &is_supported);
        break;
    case VDP_IMAGE_FORMAT_TYPE_RGBA:
        vdp_status =
            vdpau_output_surface_query_rgba_caps(driver_data,
                                                 driver_data->vdp_device,
                                                 format,
                                                 &is_supported);
        break;
    default:
        vdp_status = VDP_STATUS_INVALID_VALUE;
        break;
    }
    return vdp_status == VDP_STATUS_OK && is_supported;
}

// vaQueryImageFormats
VAStatus
vdpau_QueryImageFormats(
    VADriverContextP    ctx,
    VAImageFormat      *format_list,
    int                *num_formats
)
{
    VDPAU_DRIVER_DATA_INIT;

    if (num_formats)
        *num_formats = 0;

    if (format_list == NULL)
        return VA_STATUS_SUCCESS;

    int i, n = 0;
    for (i = 0; i < ARRAY_ELEMS(vdpau_image_formats_map); i++) {
        const vdpau_image_format_map_t * const f = &vdpau_image_formats_map[i];
        if (is_supported_format(driver_data, f->type, f->format))
            format_list[n++] = f->va_format;
    }

    /* If the assert fails then VDPAU_MAX_IMAGE_FORMATS needs to be bigger */
    ASSERT(n <= VDPAU_MAX_IMAGE_FORMATS);
    if (num_formats)
        *num_formats = n;

    return VA_STATUS_SUCCESS;
}

// vaCreateImage
VAStatus
vdpau_CreateImage(
    VADriverContextP    ctx,
    VAImageFormat      *format,
    int                 width,
    int                 height,
    VAImage            *image
)
{
    VDPAU_DRIVER_DATA_INIT;

    VdpRGBAFormat vdp_rgba_format;
    VAStatus va_status = VA_STATUS_ERROR_OPERATION_FAILED;
    unsigned int width2, height2, size2, size;
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

    if ((obj_image = VDPAU_IMAGE(image_id)) == NULL)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    obj_image->image = image;
    obj_image->vdp_rgba_surface = VDP_INVALID_HANDLE;

    size    = width * height;
    width2  = (width  + 1) / 2;
    height2 = (height + 1) / 2;
    size2   = width2 * height2;

    switch (format->fourcc) {
    case VA_FOURCC('N','V','1','2'):
        image->num_planes = 2;
        image->pitches[0] = width;
        image->offsets[0] = 0;
        image->pitches[1] = width;
        image->offsets[1] = size;
        image->data_size  = size + 2 * size2;
        break;
    case VA_FOURCC('Y','V','1','2'):
        image->num_planes = 3;
        image->pitches[0] = width;
        image->offsets[0] = 0;
        image->pitches[1] = width2;
        image->offsets[1] = size + size2;
        image->pitches[2] = width2;
        image->offsets[2] = size;
        image->data_size  = size + 2 * size2;
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
        image->num_planes = 1;
        image->pitches[0] = width * 4;
        image->offsets[0] = 0;
        image->data_size  = image->offsets[0] + image->pitches[0] * height;
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

// vaDestroyImage
VAStatus
vdpau_DestroyImage(
    VADriverContextP    ctx,
    VAImageID           image_id
)
{
    VDPAU_DRIVER_DATA_INIT;

    VAImage *image;
    object_image_p obj_image;

    if ((obj_image = VDPAU_IMAGE(image_id)) == NULL)
        return VA_STATUS_ERROR_INVALID_IMAGE;
    if ((image = obj_image->image) == NULL)
        return VA_STATUS_ERROR_INVALID_IMAGE;

    if (obj_image->vdp_rgba_surface != VDP_INVALID_HANDLE)
        vdpau_output_surface_destroy(driver_data, obj_image->vdp_rgba_surface);

    object_heap_free(&driver_data->image_heap, (object_base_p)obj_image);
    return vdpau_DestroyBuffer(ctx, image->buf);
}

// vaDeriveImage
VAStatus
vdpau_DeriveImage(
    VADriverContextP    ctx,
    VASurfaceID         surface,
    VAImage             *image
)
{
    /* TODO */
    return VA_STATUS_ERROR_OPERATION_FAILED;
}

// vaSetImagePalette
VAStatus
vdpau_SetImagePalette(
    VADriverContextP    ctx,
    VAImageID           image,
    unsigned char      *palette
)
{
    /* TODO */
    return VA_STATUS_ERROR_OPERATION_FAILED;
}

// Get image from surface
static VAStatus
get_image(
    vdpau_driver_data_t *driver_data,
    object_surface_p     obj_surface,
    object_image_p       obj_image,
    const VARectangle   *rect
)
{
    VAImage * const image = obj_image->image;
    VdpStatus vdp_status;
    uint8_t *src[3];
    unsigned int src_stride[3];
    int i;

    object_buffer_p obj_buffer = VDPAU_BUFFER(image->buf);
    if (!obj_buffer)
        return VA_STATUS_ERROR_INVALID_BUFFER;

    switch (image->format.fourcc) {
    case VA_FOURCC('Y','V','1','2'):
        /* VDPAU exposes YV12 pixels as Y/U/V planes, which turns out
           to be I420, whereas VA-API expects standard Y/V/U order */
        src[0] = (uint8_t *)obj_buffer->buffer_data + image->offsets[0];
        src_stride[0] = image->pitches[0];
        src[1] = (uint8_t *)obj_buffer->buffer_data + image->offsets[2];
        src_stride[1] = image->pitches[2];
        src[2] = (uint8_t *)obj_buffer->buffer_data + image->offsets[1];
        src_stride[2] = image->pitches[1];
        break;
    default:
        for (i = 0; i < image->num_planes; i++) {
            src[i] = (uint8_t *)obj_buffer->buffer_data + image->offsets[i];
            src_stride[i] = image->pitches[i];
        }
        break;
    }

    if (obj_image->vdp_rgba_surface == VDP_INVALID_HANDLE) {
        /* VDPAU only supports full video surface readback */
        if (rect->x != 0 ||
            rect->y != 0 ||
            obj_surface->width  != rect->width ||
            obj_surface->height != rect->height)
            return VA_STATUS_ERROR_OPERATION_FAILED;

        VdpYCbCrFormat ycbcr_format = get_VdpYCbCrFormat(&image->format);
        if (ycbcr_format == (VdpYCbCrFormat)-1)
            return VA_STATUS_ERROR_OPERATION_FAILED;

        vdp_status = vdpau_video_surface_get_bits_ycbcr(
            driver_data,
            obj_surface->vdp_surface,
            ycbcr_format,
            src, src_stride
        );
    }
    else {
        VdpRGBAFormat rgba_format = get_VdpRGBAFormat(&image->format);
        if (rgba_format == (VdpRGBAFormat)-1)
            return VA_STATUS_ERROR_OPERATION_FAILED;

        object_context_p obj_context = VDPAU_CONTEXT(obj_surface->va_context);
        if (!obj_context)
            return VA_STATUS_ERROR_INVALID_CONTEXT;

        VdpRect vdp_rect;
        vdp_rect.x0 = rect->x;
        vdp_rect.y0 = rect->y;
        vdp_rect.x1 = rect->x + rect->width;
        vdp_rect.y1 = rect->y + rect->height;
        vdp_status = vdpau_video_mixer_render(
            driver_data,
            obj_context->vdp_video_mixer,
            VDP_INVALID_HANDLE, NULL,
            VDP_VIDEO_MIXER_PICTURE_STRUCTURE_FRAME,
            0, NULL,
            obj_surface->vdp_surface,
            0, NULL,
            &vdp_rect,
            obj_image->vdp_rgba_surface,
            &vdp_rect,
            &vdp_rect,
            0, NULL
        );
        if (vdp_status != VDP_STATUS_OK)
            return vdpau_get_VAStatus(driver_data, vdp_status);

        vdp_status = vdpau_output_surface_get_bits_native(
            driver_data,
            obj_image->vdp_rgba_surface,
            &vdp_rect,
            src, src_stride
        );
    }
    return vdpau_get_VAStatus(driver_data, vdp_status);
}

// vaGetImage
VAStatus
vdpau_GetImage(
    VADriverContextP    ctx,
    VASurfaceID         surface,
    int                 x,
    int                 y,
    unsigned int        width,
    unsigned int        height,
    VAImageID           image
)
{
    VDPAU_DRIVER_DATA_INIT;

    object_surface_p obj_surface = VDPAU_SURFACE(surface);
    if (!obj_surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    object_image_p obj_image = VDPAU_IMAGE(image);
    if (!obj_image || !obj_image->image)
        return VA_STATUS_ERROR_INVALID_IMAGE;

    VARectangle rect;
    rect.x      = x;
    rect.y      = y;
    rect.width  = width;
    rect.height = height;
    return get_image(driver_data, obj_surface, obj_image, &rect);
}

// Put image to surface
static VAStatus
put_image(
    vdpau_driver_data_t *driver_data,
    object_surface_p     obj_surface,
    object_image_p       obj_image,
    const VARectangle   *src_rect,
    const VARectangle   *dst_rect
)
{
    VAImage * const image = obj_image->image;
    VdpStatus vdp_status;
    uint8_t *src[3];
    unsigned int src_stride[3];
    int i;

#if 0
    /* Don't do anything if the surface is used for rendering for example */
    /* XXX: VDPAU has no API to inform when decoding is completed... */
    if (obj_surface->va_surface_status != VASurfaceReady)
        return VA_STATUS_ERROR_SURFACE_BUSY;
#endif

    /* RGBA to video surface requires color space conversion */
    if (obj_image->vdp_rgba_surface != VDP_INVALID_HANDLE)
        return VA_STATUS_ERROR_OPERATION_FAILED;

    /* VDPAU does not support partial video surface updates */
    if (src_rect->x != 0 ||
        src_rect->y != 0 ||
        src_rect->width != image->width ||
        src_rect->height != image->height)
        return VA_STATUS_ERROR_OPERATION_FAILED;
    if (dst_rect->x != 0 ||
        dst_rect->y != 0 ||
        dst_rect->width != obj_surface->width ||
        dst_rect->height != obj_surface->height)
        return VA_STATUS_ERROR_OPERATION_FAILED;
    if (src_rect->width != dst_rect->width ||
        src_rect->height != dst_rect->height)
        return VA_STATUS_ERROR_OPERATION_FAILED;

    object_buffer_p obj_buffer = VDPAU_BUFFER(image->buf);
    if (!obj_buffer)
        return VA_STATUS_ERROR_INVALID_BUFFER;

    switch (image->format.fourcc) {
    case VA_FOURCC('Y','V','1','2'):
        /* VDPAU exposes YV12 pixels as Y/U/V planes, which turns out
           to be I420, whereas VA-API expects standard Y/V/U order */
        src[0] = (uint8_t *)obj_buffer->buffer_data + image->offsets[0];
        src_stride[0] = image->pitches[0];
        src[1] = (uint8_t *)obj_buffer->buffer_data + image->offsets[2];
        src_stride[1] = image->pitches[2];
        src[2] = (uint8_t *)obj_buffer->buffer_data + image->offsets[1];
        src_stride[2] = image->pitches[1];
        break;
    default:
        for (i = 0; i < image->num_planes; i++) {
            src[i] = (uint8_t *)obj_buffer->buffer_data + image->offsets[i];
            src_stride[i] = image->pitches[i];
        }
        break;
    }

    VdpYCbCrFormat ycbcr_format = get_VdpYCbCrFormat(&image->format);
    if (ycbcr_format == (VdpYCbCrFormat)-1)
        return VA_STATUS_ERROR_OPERATION_FAILED;

    vdp_status = vdpau_video_surface_put_bits_ycbcr(
        driver_data,
        obj_surface->vdp_surface,
        ycbcr_format,
        src, src_stride
    );
    return vdpau_get_VAStatus(driver_data, vdp_status);
}

// vaPutImage
VAStatus
vdpau_PutImage(
    VADriverContextP    ctx,
    VASurfaceID         surface,
    VAImageID           image,
    int                 src_x,
    int                 src_y,
    unsigned int        width,
    unsigned int        height,
    int                 dest_x,
    int                 dest_y
)
{
    VDPAU_DRIVER_DATA_INIT;

    object_surface_p obj_surface = VDPAU_SURFACE(surface);
    if (!obj_surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    object_image_p obj_image = VDPAU_IMAGE(image);
    if (!obj_image || !obj_image->image)
        return VA_STATUS_ERROR_INVALID_IMAGE;

    VARectangle src_rect, dst_rect;
    src_rect.x      = src_x;
    src_rect.y      = src_y;
    src_rect.width  = width;
    src_rect.height = height;
    dst_rect.x      = dest_x;
    dst_rect.y      = dest_y;
    dst_rect.width  = width;
    dst_rect.height = height;
    return put_image(driver_data, obj_surface, obj_image, &src_rect, &dst_rect);
}

// vaPutImage2
VAStatus
vdpau_PutImage_full(
    VADriverContextP    ctx,
    VASurfaceID         surface,
    VAImageID           image,
    int                 src_x,
    int                 src_y,
    unsigned int        src_width,
    unsigned int        src_height,
    int                 dest_x,
    int                 dest_y,
    unsigned int        dest_width,
    unsigned int        dest_height
)
{
    VDPAU_DRIVER_DATA_INIT;

    object_surface_p obj_surface = VDPAU_SURFACE(surface);
    if (!obj_surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    object_image_p obj_image = VDPAU_IMAGE(image);
    if (!obj_image || !obj_image->image)
        return VA_STATUS_ERROR_INVALID_IMAGE;

    VARectangle src_rect, dst_rect;
    src_rect.x      = src_x;
    src_rect.y      = src_y;
    src_rect.width  = src_width;
    src_rect.height = src_height;
    dst_rect.x      = dest_x;
    dst_rect.y      = dest_y;
    dst_rect.width  = dest_width;
    dst_rect.height = dest_height;
    return put_image(driver_data, obj_surface, obj_image, &src_rect, &dst_rect);
}
