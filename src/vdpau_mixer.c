/*
 *  vdpau_mixer.c - VDPAU backend for VA API (video mixer abstraction)
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
#include "vdpau_mixer.h"
#include "vdpau_video.h"


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

    unsigned int n = 0;
    obj_mixer->params[n]         = VDP_VIDEO_MIXER_PARAMETER_VIDEO_SURFACE_WIDTH;
    obj_mixer->param_values[n++] = &obj_mixer->width;
    obj_mixer->params[n]         = VDP_VIDEO_MIXER_PARAMETER_VIDEO_SURFACE_HEIGHT;
    obj_mixer->param_values[n++] = &obj_mixer->height;
    obj_mixer->params[n]         = VDP_VIDEO_MIXER_PARAMETER_CHROMA_TYPE;
    obj_mixer->param_values[n++] = &obj_mixer->vdp_chroma_type;
    obj_mixer->n_params          = n;

    VdpStatus status;
    status = vdpau_video_mixer_create(
        driver_data,
        driver_data->vdp_device,
        0,
        NULL,
        obj_mixer->n_params,
        obj_mixer->params,
        obj_mixer->param_values,
        &obj_mixer->vdp_video_mixer
    );
    if (status != VDP_STATUS_OK) {
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
        obj = object_heap_next(&driver_data->output_heap, &iter);
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
        destroy_output_surface(driver_data, obj_mixer);
}

VdpStatus
video_mixer_render(
    vdpau_driver_data_t *driver_data,
    object_surface_p     obj_surface,
    VdpOutputSurface     vdp_output_surface,
    const VdpRect       *vdp_src_rect,
    const VdpRect       *vdp_dst_rect
)
{
    /* Make sure there is a suitable video mixer for this surface */
    object_mixer_p obj_mixer = obj_surface->video_mixer;
    if (!obj_mixer) {
        obj_mixer = video_mixer_create_cached(driver_data, obj_surface);
        if (!obj_mixer)
            return VDP_STATUS_ERROR;
        obj_surface->video_mixer = obj_mixer;
    }

    return vdpau_video_mixer_render(
        driver_data,
        obj_mixer->vdp_video_mixer,
        VDP_INVALID_HANDLE, NULL,
        VDP_VIDEO_MIXER_PICTURE_STRUCTURE_FRAME,
        0, NULL,
        obj_surface->vdp_surface,
        0, NULL,
        vdp_src_rect,
        vdp_output_surface,
        NULL,
        vdp_dst_rect,
        0, NULL
    );
}
