/*
 *  vdpau_mixer.c - VDPAU backend for VA-API (video mixer abstraction)
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
#include "vdpau_mixer.h"
#include "vdpau_video.h"
#include <math.h>


static inline int
video_mixer_check_params(
    object_mixer_p       obj_mixer,
    object_surface_p     obj_surface
)
{
    return (obj_mixer->width == obj_surface->width &&
            obj_mixer->height == obj_surface->height &&
            obj_mixer->vdp_chroma_type == obj_surface->vdp_chroma_type);
}

static inline void
video_mixer_init_deint_surfaces(object_mixer_p obj_mixer)
{
    unsigned int i;
    for (i = 0; i < VDPAU_MAX_VIDEO_MIXER_DEINT_SURFACES; i++)
        obj_mixer->deint_surfaces[i] = VDP_INVALID_HANDLE;
}

object_mixer_p
video_mixer_create(
    vdpau_driver_data_t *driver_data,
    object_surface_p     obj_surface
)
{
    VAGenericID mixer_id = object_heap_allocate(&driver_data->mixer_heap);
    if (mixer_id == VA_INVALID_ID)
        return NULL;

    object_mixer_p obj_mixer = VDPAU_MIXER(mixer_id);
    if (!obj_mixer)
        return NULL;

    obj_mixer->refcount          = 1;
    obj_mixer->vdp_video_mixer   = VDP_INVALID_HANDLE;
    obj_mixer->width             = obj_surface->width;
    obj_mixer->height            = obj_surface->height;
    obj_mixer->vdp_chroma_type   = obj_surface->vdp_chroma_type;
    obj_mixer->vdp_colorspace    = VDP_COLOR_STANDARD_ITUR_BT_601;
    obj_mixer->vdp_procamp_mtime = 0;
    obj_mixer->vdp_bgcolor_mtime = 0;

    VdpProcamp * const procamp   = &obj_mixer->vdp_procamp;
    procamp->struct_version      = VDP_PROCAMP_VERSION;
    procamp->brightness          = 0.0;
    procamp->contrast            = 1.0;
    procamp->saturation          = 1.0;
    procamp->hue                 = 0.0;

    unsigned int n = 0;
    obj_mixer->params[n]         = VDP_VIDEO_MIXER_PARAMETER_VIDEO_SURFACE_WIDTH;
    obj_mixer->param_values[n++] = &obj_mixer->width;
    obj_mixer->params[n]         = VDP_VIDEO_MIXER_PARAMETER_VIDEO_SURFACE_HEIGHT;
    obj_mixer->param_values[n++] = &obj_mixer->height;
    obj_mixer->params[n]         = VDP_VIDEO_MIXER_PARAMETER_CHROMA_TYPE;
    obj_mixer->param_values[n++] = &obj_mixer->vdp_chroma_type;
    obj_mixer->n_params          = n;

    video_mixer_init_deint_surfaces(obj_mixer);

    VdpStatus vdp_status;
    vdp_status = vdpau_video_mixer_create(
        driver_data,
        driver_data->vdp_device,
        0,
        NULL,
        obj_mixer->n_params,
        obj_mixer->params,
        obj_mixer->param_values,
        &obj_mixer->vdp_video_mixer
    );
    if (!VDPAU_CHECK_STATUS(vdp_status, "VdpVideoMixerCreate()")) {
        video_mixer_destroy(driver_data, obj_mixer);
        return NULL;
    }
    return obj_mixer;
}

object_mixer_p
video_mixer_create_cached(
    vdpau_driver_data_t *driver_data,
    object_surface_p     obj_surface
)
{
    object_mixer_p obj_mixer = obj_surface->video_mixer;

    if (obj_mixer)
        return video_mixer_ref(driver_data, obj_mixer);

    object_heap_iterator iter;
    object_base_p obj = object_heap_first(&driver_data->mixer_heap, &iter);
    while (obj) {
        obj_mixer = (object_mixer_p)obj;
        if (video_mixer_check_params(obj_mixer, obj_surface))
            return video_mixer_ref(driver_data, obj_mixer);
        obj = object_heap_next(&driver_data->mixer_heap, &iter);
    }
    return video_mixer_create(driver_data, obj_surface);
}

void
video_mixer_destroy(
    vdpau_driver_data_t *driver_data,
    object_mixer_p       obj_mixer
)
{
    if (!obj_mixer)
        return;

    if (obj_mixer->vdp_video_mixer != VDP_INVALID_HANDLE) {
        vdpau_video_mixer_destroy(driver_data, obj_mixer->vdp_video_mixer);
        obj_mixer->vdp_video_mixer = VDP_INVALID_HANDLE;
    }
    object_heap_free(&driver_data->mixer_heap, (object_base_p)obj_mixer);
}

object_mixer_p
video_mixer_ref(
    vdpau_driver_data_t *driver_data,
    object_mixer_p       obj_mixer
)
{
    if (obj_mixer)
        ++obj_mixer->refcount;
    return obj_mixer;
}

void
video_mixer_unref(
    vdpau_driver_data_t *driver_data,
    object_mixer_p       obj_mixer
)
{
    if (obj_mixer && --obj_mixer->refcount == 0)
        video_mixer_destroy(driver_data, obj_mixer);
}

static VdpStatus
video_mixer_update_csc_matrix(
    vdpau_driver_data_t *driver_data,
    object_mixer_p       obj_mixer,
    VdpColorStandard     vdp_colorspace
)
{
    uint64_t new_mtime = obj_mixer->vdp_procamp_mtime;
    unsigned int i;

    for (i = 0; i < driver_data->va_display_attrs_count; i++) {
        VADisplayAttribute * const attr = &driver_data->va_display_attrs[i];
        if (obj_mixer->vdp_procamp_mtime >= driver_data->va_display_attrs_mtime[i])
            continue;

        float *vp, v = attr->value / 100.0;
        switch (attr->type) {
        case VADisplayAttribBrightness: /* VDPAU range: -1.0 to 1.0 */
            vp = &obj_mixer->vdp_procamp.brightness;
            break;
        case VADisplayAttribContrast:   /* VDPAU range: 0.0 to 10.0 */
            vp = &obj_mixer->vdp_procamp.contrast;
            goto do_range_0_10;
        case VADisplayAttribSaturation: /* VDPAU range: 0.0 to 10.0 */
            vp = &obj_mixer->vdp_procamp.saturation;
        do_range_0_10:
            if (attr->value > 0) /* scale VA:0-100 to VDPAU:1.0-10.0 */
                v *= 9.0;
            v += 1.0;
            break;
        case VADisplayAttribHue:        /* VDPAU range: -PI to PI */
            vp = &obj_mixer->vdp_procamp.hue;
            v *= M_PI;
            break;
        default:
            vp = NULL;
            break;
        }

        if (vp) {
            *vp = v;
            if (new_mtime < driver_data->va_display_attrs_mtime[i])
                new_mtime = driver_data->va_display_attrs_mtime[i];
        }
    }

    /* Commit changes, if any */
    if (new_mtime > obj_mixer->vdp_procamp_mtime || vdp_colorspace != obj_mixer->vdp_colorspace) {
        VdpCSCMatrix vdp_matrix;
        VdpStatus vdp_status;
        static const VdpVideoMixerAttribute attrs[1] = { VDP_VIDEO_MIXER_ATTRIBUTE_CSC_MATRIX };
        const void *attr_values[1] = { &vdp_matrix };

        vdp_status = vdpau_generate_csc_matrix(
            driver_data,
            &obj_mixer->vdp_procamp,
            vdp_colorspace,
            &vdp_matrix
        );
        if (!VDPAU_CHECK_STATUS(vdp_status, "VdpGenerateCSCMatrix()"))
            return vdp_status;

        vdp_status = vdpau_video_mixer_set_attribute_values(
            driver_data,
            obj_mixer->vdp_video_mixer,
            1, attrs, attr_values
        );
        if (!VDPAU_CHECK_STATUS(vdp_status, "VdpVideoMixerSetAttributeValues()"))
            return vdp_status;

        obj_mixer->vdp_colorspace    = vdp_colorspace;
        obj_mixer->vdp_procamp_mtime = new_mtime;
    }
    return VDP_STATUS_OK;
}

VdpStatus
video_mixer_set_background_color(
    vdpau_driver_data_t *driver_data,
    object_mixer_p       obj_mixer,
    const VdpColor      *vdp_color
)
{
    VdpStatus vdp_status;
    VdpVideoMixerAttribute attrs[1];
    const void *attr_values[1];

    attrs[0] = VDP_VIDEO_MIXER_ATTRIBUTE_BACKGROUND_COLOR;
    attr_values[0] = vdp_color;

    vdp_status = vdpau_video_mixer_set_attribute_values(
        driver_data,
        obj_mixer->vdp_video_mixer,
        1, attrs, attr_values
    );
    return vdp_status;
}

static VdpStatus
video_mixer_update_background_color(
    vdpau_driver_data_t *driver_data,
    object_mixer_p       obj_mixer
)
{
    unsigned int i;
    for (i = 0; i < driver_data->va_display_attrs_count; i++) {
        VADisplayAttribute * const attr = &driver_data->va_display_attrs[i];
        if (attr->type != VADisplayAttribBackgroundColor)
            continue;

        if (obj_mixer->vdp_bgcolor_mtime < driver_data->va_display_attrs_mtime[i]) {
            VdpStatus vdp_status;
            VdpColor vdp_color;

            vdp_color.red   = ((attr->value >> 16) & 0xff) / 255.0f;
            vdp_color.green = ((attr->value >> 8) & 0xff) / 255.0f;
            vdp_color.blue  = (attr->value & 0xff)/ 255.0f;
            vdp_color.alpha = 1.0f;

            vdp_status = video_mixer_set_background_color(
                driver_data,
                obj_mixer,
                &vdp_color
            );
            if (vdp_status != VDP_STATUS_OK)
                return vdp_status;

            obj_mixer->vdp_bgcolor_mtime = driver_data->va_display_attrs_mtime[i];
            break;
        }
    }
    return VDP_STATUS_OK;
}

static inline void
video_mixer_push_deint_surface(
    object_mixer_p   obj_mixer,
    object_surface_p obj_surface
)
{
    unsigned int i;
    for (i = VDPAU_MAX_VIDEO_MIXER_DEINT_SURFACES - 1; i >= 1; i--)
        obj_mixer->deint_surfaces[i] = obj_mixer->deint_surfaces[i - 1];
    obj_mixer->deint_surfaces[0] = obj_surface->vdp_surface;
}

VdpStatus
video_mixer_render(
    vdpau_driver_data_t *driver_data,
    object_mixer_p       obj_mixer,
    object_surface_p     obj_surface,
    VdpOutputSurface     vdp_background,
    VdpOutputSurface     vdp_output_surface,
    const VdpRect       *vdp_src_rect,
    const VdpRect       *vdp_dst_rect,
    unsigned int         flags
)
{
    VdpColorStandard colorspace;
    if (flags & VA_SRC_BT709)
        colorspace = VDP_COLOR_STANDARD_ITUR_BT_709;
    else
        colorspace = VDP_COLOR_STANDARD_ITUR_BT_601;

    VdpStatus vdp_status;
    vdp_status = video_mixer_update_csc_matrix(
        driver_data,
        obj_mixer,
        colorspace
    );
    if (vdp_status != VDP_STATUS_OK)
        return vdp_status;

    vdp_status = video_mixer_update_background_color(driver_data, obj_mixer);
    if (vdp_status != VDP_STATUS_OK)
        return vdp_status;

    VdpVideoMixerPictureStructure field;
    switch (flags & (VA_TOP_FIELD|VA_BOTTOM_FIELD)) {
    case VA_TOP_FIELD:
        field = VDP_VIDEO_MIXER_PICTURE_STRUCTURE_TOP_FIELD;
        break;
    case VA_BOTTOM_FIELD:
        field = VDP_VIDEO_MIXER_PICTURE_STRUCTURE_BOTTOM_FIELD;
        break;
    default:
        field = VDP_VIDEO_MIXER_PICTURE_STRUCTURE_FRAME;
        break;
    }
    video_mixer_push_deint_surface(obj_mixer, obj_surface);

    if (flags & VA_CLEAR_DRAWABLE)
        vdp_background = VDP_INVALID_HANDLE;

    vdp_status = vdpau_video_mixer_render(
        driver_data,
        obj_mixer->vdp_video_mixer,
        vdp_background, NULL,
        field,
        VDPAU_MAX_VIDEO_MIXER_DEINT_SURFACES - 1, &obj_mixer->deint_surfaces[1],
        obj_mixer->deint_surfaces[0],
        0, NULL,
        vdp_src_rect,
        vdp_output_surface,
        NULL,
        vdp_dst_rect,
        0, NULL
    );
    video_mixer_push_deint_surface(obj_mixer, obj_surface);
    return vdp_status;
}
