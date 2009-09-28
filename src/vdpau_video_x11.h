/*
 *  vdpau_video_x11.h - VDPAU backend for VA API (X11 rendering)
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

#ifndef VDPAU_VIDEO_X11_H
#define VDPAU_VIDEO_X11_H

#include "vdpau_driver.h"

typedef struct object_output object_output_t;
struct object_output {
    struct object_base          base;
    Drawable                    drawable;
    uint32_t                    width;
    uint32_t                    height;
    VdpPresentationQueue        vdp_flip_queue;
    VdpPresentationQueueTarget  vdp_flip_target;
    uint32_t                    output_surface_width;
    uint32_t                    output_surface_height;
    VdpOutputSurface            vdp_output_surfaces[VDPAU_MAX_OUTPUT_SURFACES];
    int                         current_output_surface;
};

// Create output surface
VASurfaceID
create_output_surface(
    vdpau_driver_data_t *driver_data,
    uint32_t             width,
    uint32_t             height
) attribute_hidden;

// Destroy output surface
void
destroy_output_surface(vdpau_driver_data_t *driver_data, VASurfaceID surface)
    attribute_hidden;

// Create presentation queue
VAStatus
create_flip_queue(vdpau_driver_data_t *driver_data, object_output_p obj_output)
    attribute_hidden;

// Destroy presentation queue
void
destroy_flip_queue(vdpau_driver_data_t *driver_data, object_output_p obj_output)
    attribute_hidden;

// Render surface to a Drawable
VAStatus
put_surface(
    vdpau_driver_data_t *driver_data,
    VASurfaceID          surface,
    Drawable             drawable,
    unsigned int         drawable_width,
    unsigned int         drawable_height,
    const VARectangle   *source_rect,
    const VARectangle   *target_rect
) attribute_hidden;

// vaPutSurface
VAStatus
vdpau_PutSurface(
    VADriverContextP    ctx,
    VASurfaceID         surface,
    Drawable            draw,
    short               srcx,
    short               srcy,
    unsigned short      srcw,
    unsigned short      srch,
    short               destx,
    short               desty,
    unsigned short      destw,
    unsigned short      desth,
    VARectangle        *cliprects,
    unsigned int        number_cliprects,
    unsigned int        flags
) attribute_hidden;

#endif /* VDPAU_VIDEO_X11_H */
