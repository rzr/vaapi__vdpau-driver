/*
 *  vdpau_mixer.h - VDPAU backend for VA API (video mixer abstraction)
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

#ifndef VDPAU_MIXER_H
#define VDPAU_MIXER_H

#include "vdpau_driver.h"

typedef struct object_mixer object_mixer_t;
struct object_mixer {
    struct object_base          base;
    unsigned int                refcount;
    VdpVideoMixer               vdp_video_mixer;
    VdpChromaType               vdp_chroma_type;
    unsigned int                width;
    unsigned int                height;
    VdpVideoMixerParameter      params[VDPAU_MAX_VIDEO_MIXER_PARAMS];
    void                       *param_values[VDPAU_MAX_VIDEO_MIXER_PARAMS];
    unsigned int                n_params;
    VdpColorStandard            vdp_colorspace;
    VdpProcamp                  vdp_procamp;
    uint64_t                    vdp_procamp_mtime;
};

object_mixer_p
video_mixer_create(
    vdpau_driver_data_t *driver_data,
    object_surface_p     obj_surface
) attribute_hidden;

object_mixer_p
video_mixer_create_cached(
    vdpau_driver_data_t *driver_data,
    object_surface_p     obj_surface
) attribute_hidden;

void
video_mixer_destroy(
    vdpau_driver_data_t *driver_data,
    object_mixer_p       obj_mixer
) attribute_hidden;

object_mixer_p
video_mixer_ref(
    vdpau_driver_data_t *driver_data,
    object_mixer_p       obj_mixer
) attribute_hidden;

void
video_mixer_unref(
    vdpau_driver_data_t *driver_data,
    object_mixer_p       obj_mixer
) attribute_hidden;

VdpStatus
video_mixer_render(
    vdpau_driver_data_t *driver_data,
    object_surface_p     obj_surface,
    VdpOutputSurface     vdp_output_surface,
    const VdpRect       *vdp_src_rect,
    const VdpRect       *vdp_dst_rect,
    unsigned int         flags
) attribute_hidden;

#endif /* VDPAU_MIXER_H */
