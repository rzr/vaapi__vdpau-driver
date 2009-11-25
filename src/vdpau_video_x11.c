/*
 *  vdpau_video.h - VDPAU backend for VA API (X11 rendering)
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
#include "vdpau_video.h"
#include "vdpau_video_x11.h"
#include "vdpau_subpic.h"
#include "vdpau_mixer.h"
#include "utils.h"

#define DEBUG 1
#include "debug.h"


// Returns X drawable dimensions
static int
get_drawable_size(
    Display      *display,
    Drawable      drawable,
    unsigned int *pwidth,
    unsigned int *pheight
)
{
    Window rootwin;
    int x, y;
    unsigned int width, height, border_width, depth;

    x11_trap_errors();
    XGetGeometry(display, drawable, &rootwin,
                 &x, &y, &width, &height, &border_width, &depth);
    if (x11_untrap_errors() != 0)
        return -1;

    if (pwidth)
        *pwidth = width;

    if (pheight)
        *pheight = height;

    return 0;
}

// Ensure output surface size matches drawable size
static int
output_surface_ensure_size(
    vdpau_driver_data_t *driver_data,
    object_output_p      obj_output,
    unsigned int         width,
    unsigned int         height
)
{
    if (!obj_output)
        return -1;

    if (width <= obj_output->max_width && height <= obj_output->max_height)
        return 0;

    const unsigned int max_waste = 1U << 8;
    unsigned int new_max_width   = (width  + max_waste - 1) & -max_waste;
    unsigned int new_max_height  = (height + max_waste - 1) & -max_waste;

    unsigned int i;
    for (i = 0; i < VDPAU_MAX_OUTPUT_SURFACES; i++) {
        if (obj_output->vdp_output_surfaces[i] != VDP_INVALID_HANDLE) {
            vdpau_output_surface_destroy(
                driver_data,
                obj_output->vdp_output_surfaces[i]
            );
            obj_output->vdp_output_surfaces[i] = VDP_INVALID_HANDLE;
        }

        VdpStatus vdp_status;
        vdp_status = vdpau_output_surface_create(
            driver_data,
            driver_data->vdp_device,
            VDP_RGBA_FORMAT_B8G8R8A8,
            new_max_width,
            new_max_height,
            &obj_output->vdp_output_surfaces[i]
        );
        if (vdp_status != VDP_STATUS_OK)
            return -1;
    }

    obj_output->width      = width;
    obj_output->height     = height;
    obj_output->max_width  = new_max_width;
    obj_output->max_height = new_max_height;
    return 0;
}

// Create output surface
static object_output_p
output_surface_create(
    vdpau_driver_data_t *driver_data,
    Drawable             drawable,
    unsigned int         width,
    unsigned int         height
)
{
    VdpStatus vdp_status;
    VASurfaceID surface = object_heap_allocate(&driver_data->output_heap);
    if (surface == VA_INVALID_ID)
        return NULL;

    object_output_p obj_output = VDPAU_OUTPUT(surface);
    ASSERT(obj_output);
    if (!obj_output)
        return NULL;

    obj_output->refcount                = 1;
    obj_output->drawable                = drawable;
    obj_output->width                   = width;
    obj_output->height                  = height;
    obj_output->max_width               = 0;
    obj_output->max_height              = 0;
    obj_output->vdp_flip_queue          = VDP_INVALID_HANDLE;
    obj_output->vdp_flip_target         = VDP_INVALID_HANDLE;
    obj_output->current_output_surface  = 0;

    unsigned int i;
    for (i = 0; i < VDPAU_MAX_OUTPUT_SURFACES; i++)
        obj_output->vdp_output_surfaces[i] = VDP_INVALID_HANDLE;

    if (drawable != None) {
        vdp_status = vdpau_presentation_queue_target_create_x11(
            driver_data,
            driver_data->vdp_device,
            obj_output->drawable,
            &obj_output->vdp_flip_target
        );
        if (vdp_status != VDP_STATUS_OK) {
            output_surface_destroy(driver_data, obj_output);
            return NULL;
        }

        vdp_status = vdpau_presentation_queue_create(
            driver_data,
            driver_data->vdp_device,
            obj_output->vdp_flip_target,
            &obj_output->vdp_flip_queue
        );
        if (vdp_status != VDP_STATUS_OK) {
            output_surface_destroy(driver_data, obj_output);
            return NULL;
        }
    }

    if (output_surface_ensure_size(driver_data, obj_output, width, height) < 0) {
        output_surface_destroy(driver_data, obj_output);
        return NULL;
    }
    return obj_output;
}

// Destroy output surface
void
output_surface_destroy(
    vdpau_driver_data_t *driver_data,
    object_output_p      obj_output
)
{
    if (!obj_output)
        return;

    if (obj_output->vdp_flip_queue != VDP_INVALID_HANDLE) {
        vdpau_presentation_queue_destroy(
            driver_data,
            obj_output->vdp_flip_queue
        );
        obj_output->vdp_flip_queue = VDP_INVALID_HANDLE;
    }

    if (obj_output->vdp_flip_target != VDP_INVALID_HANDLE) {
        vdpau_presentation_queue_target_destroy(
            driver_data,
            obj_output->vdp_flip_target
        );
        obj_output->vdp_flip_target = VDP_INVALID_HANDLE;
    }

    unsigned int i;
    for (i = 0; i < VDPAU_MAX_OUTPUT_SURFACES; i++) {
        VdpOutputSurface vdp_output_surface;
        vdp_output_surface = obj_output->vdp_output_surfaces[i];
        if (vdp_output_surface != VDP_INVALID_HANDLE) {
            vdpau_output_surface_destroy(driver_data, vdp_output_surface);
            obj_output->vdp_output_surfaces[i] = VDP_INVALID_HANDLE;
        }
    }
    object_heap_free(&driver_data->output_heap, (object_base_p)obj_output);
}

// Reference output surface
object_output_p
output_surface_ref(
    vdpau_driver_data_t *driver_data,
    object_output_p      obj_output
)
{
    if (obj_output)
        ++obj_output->refcount;
    return obj_output;
}

// Unreference output surface, destroying the surface if refcount reaches zero
void
output_surface_unref(
    vdpau_driver_data_t *driver_data,
    object_output_p      obj_output
)
{
    if (obj_output && --obj_output->refcount == 0)
        output_surface_destroy(driver_data, obj_output);
}

// Ensure an output surface is created for the specified surface and drawable
static object_output_p
output_surface_ensure(
    vdpau_driver_data_t *driver_data,
    object_surface_p     obj_surface,
    Drawable             drawable,
    unsigned int         width,
    unsigned int         height
)
{
    if (!obj_surface)
        return NULL;

    /* Check for a output surface matching Drawable */
    if (!obj_surface->output_surface) {
        object_heap_iterator iter;
        object_base_p obj = object_heap_first(&driver_data->output_heap, &iter);
        while (obj) {
            object_output_p obj_output = (object_output_p)obj;
            if (obj_output->drawable == drawable) {
                obj_surface->output_surface = output_surface_ref(
                    driver_data,
                    obj_output
                );
                break;
            }
            obj = object_heap_next(&driver_data->output_heap, &iter);
        }
    }

    if (obj_surface->output_surface &&
        obj_surface->output_surface->drawable != drawable) {
        output_surface_unref(driver_data, obj_surface->output_surface);
        obj_surface->output_surface = NULL;
    }

    /* Fallback: create a new output surface */
    if (!obj_surface->output_surface)
        obj_surface->output_surface = output_surface_create(driver_data,
                                                            drawable,
                                                            width, height);

    return obj_surface->output_surface;
}

// Ensure rectangle is within specified bounds
static inline void
ensure_bounds(VdpRect *rect, unsigned int width, unsigned int height)
{
    rect->x0 = MAX(rect->x0, 0);
    rect->y0 = MAX(rect->y0, 0);
    rect->x1 = MIN(rect->x1, width);
    rect->y1 = MIN(rect->y1, height);
}

// Render surface to the VDPAU output surface
static VAStatus
render_surface(
    vdpau_driver_data_t *driver_data,
    object_surface_p     obj_surface,
    object_output_p      obj_output,
    const VARectangle   *source_rect,
    const VARectangle   *target_rect,
    unsigned int         flags
)
{
    VdpRect src_rect;
    src_rect.x0 = source_rect->x;
    src_rect.y0 = source_rect->y;
    src_rect.x1 = source_rect->x + source_rect->width;
    src_rect.y1 = source_rect->y + source_rect->height;
    ensure_bounds(&src_rect, obj_surface->width, obj_surface->height);   

    VdpRect dst_rect;
    dst_rect.x0 = target_rect->x;
    dst_rect.y0 = target_rect->y;
    dst_rect.x1 = target_rect->x + target_rect->width;
    dst_rect.y1 = target_rect->y + target_rect->height;
    ensure_bounds(&dst_rect, obj_output->width, obj_output->height);

    VdpStatus vdp_status = video_mixer_render(
        driver_data,
        obj_surface,
        obj_output->vdp_output_surfaces[obj_output->current_output_surface],
        &src_rect,
        &dst_rect,
        flags
    );
    return vdpau_get_VAStatus(driver_data, vdp_status);
}

// Render subpictures to the VDPAU output surface
static VAStatus
render_subpicture(
    vdpau_driver_data_t         *driver_data,
    object_subpicture_p          obj_subpicture,
    object_surface_p             obj_surface,
    object_output_p              obj_output,
    const VARectangle           *output_rect,
    const SubpictureAssociationP assoc
)
{
    VAStatus va_status = commit_subpicture(driver_data, obj_subpicture);
    if (va_status != VA_STATUS_SUCCESS)
        return va_status;

    object_image_p obj_image = VDPAU_IMAGE(obj_subpicture->image_id);
    if (!obj_image)
        return VA_STATUS_ERROR_INVALID_IMAGE;

    const float psx = (float)obj_surface->width  / (float)obj_subpicture->width;
    const float psy = (float)obj_surface->height / (float)obj_subpicture->height;
    const float ssx = (float)output_rect->width  / (float)obj_surface->width;
    const float ssy = (float)output_rect->height / (float)obj_surface->height;
    const float sx  = psx * ssx;
    const float sy  = psy * ssy;

    VdpRect src_rect;
    src_rect.x0 = assoc->src_rect.x;
    src_rect.y0 = assoc->src_rect.y;
    src_rect.x1 = src_rect.x0 + assoc->src_rect.width;
    src_rect.y1 = src_rect.y0 + assoc->src_rect.height;
    ensure_bounds(&src_rect, obj_subpicture->width, obj_subpicture->height);

    VdpRect dst_rect;
    dst_rect.x0 = output_rect->x + sx * (float)assoc->dst_rect.x;
    dst_rect.y0 = output_rect->y + sx * (float)assoc->dst_rect.y;
    dst_rect.x1 = dst_rect.x0 + sx * (float)assoc->dst_rect.width;
    dst_rect.y1 = dst_rect.y0 + sy * (float)assoc->dst_rect.height;
    ensure_bounds(&dst_rect, obj_output->width, obj_output->height);

    VdpOutputSurfaceRenderBlendState blend_state;
    blend_state.struct_version                 = VDP_OUTPUT_SURFACE_RENDER_BLEND_STATE_VERSION;
    blend_state.blend_factor_source_color      = VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_ONE;
    blend_state.blend_factor_source_alpha      = VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_ONE;
    blend_state.blend_factor_destination_color = VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_state.blend_factor_destination_alpha = VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_state.blend_equation_color           = VDP_OUTPUT_SURFACE_RENDER_BLEND_EQUATION_ADD;
    blend_state.blend_equation_alpha           = VDP_OUTPUT_SURFACE_RENDER_BLEND_EQUATION_ADD;

    VdpStatus vdp_status;
    switch (obj_image->vdp_format_type) {
    case VDP_IMAGE_FORMAT_TYPE_RGBA:
        vdp_status = vdpau_output_surface_render_bitmap_surface(
            driver_data,
            obj_output->vdp_output_surfaces[obj_output->current_output_surface],
            &dst_rect,
            obj_subpicture->vdp_bitmap_surface,
            &src_rect,
            NULL,
            &blend_state,
            VDP_OUTPUT_SURFACE_RENDER_ROTATE_0
        );
        break;
    case VDP_IMAGE_FORMAT_TYPE_INDEXED:
        vdp_status = vdpau_output_surface_render_output_surface(
            driver_data,
            obj_output->vdp_output_surfaces[obj_output->current_output_surface],
            &dst_rect,
            obj_subpicture->vdp_output_surface,
            &src_rect,
            NULL,
            &blend_state,
            VDP_OUTPUT_SURFACE_RENDER_ROTATE_0
        );
        break;
    default:
        vdp_status = VDP_STATUS_ERROR;
        break;
    }
    return vdpau_get_VAStatus(driver_data, vdp_status);
}

static VAStatus
render_subpictures(
    vdpau_driver_data_t *driver_data,
    object_surface_p     obj_surface,
    object_output_p      obj_output,
    const VARectangle   *output_rect
)
{
    unsigned int i;
    for (i = 0; i < obj_surface->assocs_count; i++) {
        SubpictureAssociationP const assoc = obj_surface->assocs[i];
        ASSERT(assoc);
        if (!assoc)
            continue;

        object_subpicture_p obj_subpicture;
        obj_subpicture = VDPAU_SUBPICTURE(assoc->subpicture);
        ASSERT(obj_subpicture);
        if (!obj_subpicture)
            continue;

        VAStatus va_status = render_subpicture(
            driver_data,
            obj_subpicture,
            obj_surface,
            obj_output,
            output_rect,
            assoc
        );
        if (va_status != VA_STATUS_SUCCESS)
            return va_status;
    }
    return VA_STATUS_SUCCESS;
}

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
)
{
    VdpStatus vdp_status;
    VAStatus va_status;

    object_surface_p obj_surface = VDPAU_SURFACE(surface);
    ASSERT(obj_surface);
    if (!obj_surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    object_output_p obj_output;
    obj_output = output_surface_ensure(
        driver_data,
        obj_surface,
        drawable,
        drawable_width,
        drawable_height
    );
    ASSERT(obj_output);
    if (!obj_output)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    ASSERT(obj_output->drawable == drawable);
    ASSERT(obj_output->vdp_flip_queue != VDP_INVALID_HANDLE);
    ASSERT(obj_output->vdp_flip_target != VDP_INVALID_HANDLE);

    obj_surface->va_surface_status  = VASurfaceReady;
    obj_surface->vdp_output_surface = VDP_INVALID_HANDLE;

    /* Wait for the output surface to be ready.
       i.e. it completed the previous rendering */
    VdpTime dummy_time;
    vdp_status = vdpau_presentation_queue_block_until_surface_idle(
        driver_data,
        obj_output->vdp_flip_queue,
        obj_output->vdp_output_surfaces[obj_output->current_output_surface],
        &dummy_time
    );
    if (vdp_status != VDP_STATUS_OK)
        return vdpau_get_VAStatus(driver_data, vdp_status);

    if (output_surface_ensure_size(driver_data, obj_output,
                                   drawable_width, drawable_height) < 0)
        return VA_STATUS_ERROR_OPERATION_FAILED;

    /* Render the video surface to the output surface */
    va_status = render_surface(
        driver_data,
        obj_surface,
        obj_output,
        source_rect,
        target_rect,
        flags
    );
    if (va_status != VA_STATUS_SUCCESS)
        return va_status;

    /* Render subpictures to the output surface, applying scaling */
    va_status = render_subpictures(
        driver_data,
        obj_surface,
        obj_output,
        target_rect
    );
    if (va_status != VA_STATUS_SUCCESS)
        return va_status;

    /* Queue surface for display */
    vdp_status  = vdpau_presentation_queue_display(
        driver_data,
        obj_output->vdp_flip_queue,
        obj_output->vdp_output_surfaces[obj_output->current_output_surface],
        drawable_width,
        drawable_height,
        0
    );
    if (vdp_status != VDP_STATUS_OK)
        return vdpau_get_VAStatus(driver_data, vdp_status);

    obj_surface->va_surface_status     = VASurfaceDisplaying;
    obj_surface->vdp_output_surface    =
        obj_output->vdp_output_surfaces[obj_output->current_output_surface];
    obj_output->current_output_surface =
        (obj_output->current_output_surface + 1) % VDPAU_MAX_OUTPUT_SURFACES;
    return VA_STATUS_SUCCESS;
}

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
)
{
    VDPAU_DRIVER_DATA_INIT;

    /* XXX: no clip rects supported */
    if (cliprects || number_cliprects > 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    unsigned int w, h;
    if (get_drawable_size(ctx->x11_dpy, draw, &w, &h) < 0)
        return VA_STATUS_ERROR_OPERATION_FAILED;

    VARectangle src_rect, dst_rect;
    src_rect.x      = srcx;
    src_rect.y      = srcy;
    src_rect.width  = srcw;
    src_rect.height = srch;
    dst_rect.x      = destx;
    dst_rect.y      = desty;
    dst_rect.width  = destw;
    dst_rect.height = desth;
    return put_surface(driver_data, surface, draw, w, h, &src_rect, &dst_rect, flags);
}
