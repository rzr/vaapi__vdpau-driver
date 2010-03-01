/*
 *  vdpau_video_x11.h - VDPAU backend for VA API (X11 rendering)
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

#ifndef VDPAU_VIDEO_X11_H
#define VDPAU_VIDEO_X11_H

#include "vdpau_driver.h"
#include <pthread.h>
#include "uasyncqueue.h"

typedef struct object_output object_output_t;
struct object_output {
    struct object_base          base;
    unsigned int                refcount;
    Drawable                    drawable;
    unsigned int                width;
    unsigned int                height;
    unsigned int                max_width;
    unsigned int                max_height;
    VdpPresentationQueue        vdp_flip_queue;
    VdpPresentationQueueTarget  vdp_flip_target;
    VdpOutputSurface            vdp_output_surfaces[VDPAU_MAX_OUTPUT_SURFACES];
    unsigned int                current_output_surface;
    unsigned int                queued_surfaces;
    unsigned int                fields;
    UAsyncQueue                *render_comm;
    pthread_t                   render_thread;
    unsigned int                render_thread_ok;
};

// Destroy output surface
void
output_surface_destroy(
    vdpau_driver_data_t *driver_data,
    object_output_p      obj_output
) attribute_hidden;

// Reference output surface
object_output_p
output_surface_ref(
    vdpau_driver_data_t *driver_data,
    object_output_p      obj_output
) attribute_hidden;

// Unreference output surface
// NOTE: this destroys the surface if refcount reaches zero
void
output_surface_unref(
    vdpau_driver_data_t *driver_data,
    object_output_p      obj_output
) attribute_hidden;

// Render surface to a Drawable
VAStatus
put_surface(
    vdpau_driver_data_t *driver_data,
    VASurfaceID          surface,
    Drawable             drawable,
    unsigned int         drawable_width,
    unsigned int         drawable_height,
    const VARectangle   *source_rect,
    const VARectangle   *target_rect,
    unsigned int         flags
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
