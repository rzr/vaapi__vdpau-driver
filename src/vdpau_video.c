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

#include "sysdeps.h"
#include "vdpau_video.h"
#include "vdpau_video_x11.h"
#include "vdpau_decode.h"
#include "vdpau_subpic.h"
#include "vdpau_mixer.h"
#include "utils.h"

#define DEBUG 1
#include "debug.h"


/* Define wait delay (in microseconds) for vaSyncSurface() implementation
   with polling. */
#define VDPAU_SYNC_DELAY 5000

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


/* ====================================================================== */
/* === VA API Implementation with VDPAU                               === */
/* ====================================================================== */

// Returns the maximum dimensions supported by the VDPAU implementation for that profile
static inline VdpBool
get_max_surface_size(
    vdpau_driver_data_t *driver_data,
    VdpDecoderProfile    profile,
    uint32_t            *pmax_width,
    uint32_t            *pmax_height
)
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

// vaGetConfigAttributes
VAStatus
vdpau_GetConfigAttributes(
    VADriverContextP    ctx,
    VAProfile           profile,
    VAEntrypoint        entrypoint,
    VAConfigAttrib     *attrib_list,
    int                 num_attribs
)
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
VAStatus vdpau_DestroyConfig(VADriverContextP ctx, VAConfigID config_id)
{
    VDPAU_DRIVER_DATA_INIT;

    object_config_p obj_config = VDPAU_CONFIG(config_id);
    ASSERT(obj_config);
    if (obj_config == NULL)
        return VA_STATUS_ERROR_INVALID_CONFIG;

    object_heap_free(&driver_data->config_heap, (object_base_p)obj_config);
    return VA_STATUS_SUCCESS;
}

// vaCreateConfig
VAStatus
vdpau_CreateConfig(
    VADriverContextP    ctx,
    VAProfile           profile,
    VAEntrypoint        entrypoint,
    VAConfigAttrib     *attrib_list,
    int                 num_attribs,
    VAConfigID         *config_id
)
{
    VDPAU_DRIVER_DATA_INIT;

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
    if ((obj_config = VDPAU_CONFIG(configID)) == NULL)
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
VAStatus
vdpau_QueryConfigAttributes(
    VADriverContextP    ctx,
    VAConfigID          config_id,
    VAProfile          *profile,
    VAEntrypoint       *entrypoint,
    VAConfigAttrib     *attrib_list,
    int                *num_attribs
)
{
    VDPAU_DRIVER_DATA_INIT;

    VAStatus va_status = VA_STATUS_SUCCESS;
    object_config_p obj_config;
    int i;

    obj_config = VDPAU_CONFIG(config_id);
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

// Add subpicture association to surface
// NOTE: the subpicture owns the SubpictureAssociation object
int surface_add_association(
    object_surface_p            obj_surface,
    SubpictureAssociationP      assoc
)
{
    /* Check that we don't already have this association */
    if (obj_surface->assocs) {
        unsigned int i;
        for (i = 0; i < obj_surface->assocs_count; i++) {
            if (obj_surface->assocs[i] == assoc)
                return 0;
            if (obj_surface->assocs[i]->surface == assoc->surface) {
                /* XXX: this should not happen, but replace it in the interim */
                ASSERT(obj_surface->assocs[i]->surface != assoc->surface);
                obj_surface->assocs[i] = assoc;
                return 0;
            }
        }
    }

    /* Check that we have not reached the maximum subpictures capacity yet */
    if (obj_surface->assocs_count >= VDPAU_MAX_SUBPICTURES)
        return -1;

    /* Append this subpicture association */
    SubpictureAssociationP *assocs;
    assocs = realloc_buffer(&obj_surface->assocs,
                            &obj_surface->assocs_count_max,
                            1 + obj_surface->assocs_count,
                            sizeof(obj_surface->assocs[0]));
    if (!assocs)
        return -1;

    assocs[obj_surface->assocs_count++] = assoc;
    return 0;
}

// Remove subpicture association from surface
// NOTE: the subpicture owns the SubpictureAssociation object
int surface_remove_association(
    object_surface_p            obj_surface,
    SubpictureAssociationP      assoc
)
{
    if (!obj_surface->assocs || obj_surface->assocs_count == 0)
        return -1;

    unsigned int i;
    const unsigned int last = obj_surface->assocs_count - 1;
    for (i = 0; i <= last; i++) {
        if (obj_surface->assocs[i] == assoc) {
            /* Swap with the last subpicture */
            obj_surface->assocs[i] = obj_surface->assocs[last];
            obj_surface->assocs[last] = NULL;
            obj_surface->assocs_count--;
            return 0;
        }
    }
    return -1;
}

// vaDestroySurfaces
VAStatus
vdpau_DestroySurfaces(
    VADriverContextP    ctx,
    VASurfaceID        *surface_list,
    int                 num_surfaces
)
{
    VDPAU_DRIVER_DATA_INIT;

    int i, j, n;
    for (i = num_surfaces - 1; i >= 0; i--) {
        object_surface_p obj_surface = VDPAU_SURFACE(surface_list[i]);
        ASSERT(obj_surface);

        if (obj_surface->vdp_surface != VDP_INVALID_HANDLE) {
            vdpau_video_surface_destroy(driver_data, obj_surface->vdp_surface);
            obj_surface->vdp_surface = VDP_INVALID_HANDLE;
        }

        if (obj_surface->output_surface) {
            output_surface_unref(driver_data, obj_surface->output_surface);
            obj_surface->output_surface = NULL;
        }

        if (obj_surface->video_mixer) {
            video_mixer_unref(driver_data, obj_surface->video_mixer);
            obj_surface->video_mixer = NULL;
        }

        if (obj_surface->assocs) {
            object_subpicture_p obj_subpicture;
            VAStatus status;
            const unsigned int n_assocs = obj_surface->assocs_count;

            for (j = 0, n = 0; j < n_assocs; j++) {
                SubpictureAssociationP const assoc = obj_surface->assocs[0];
                if (!assoc)
                    continue;
                obj_subpicture = VDPAU_SUBPICTURE(assoc->subpicture);
                ASSERT(obj_subpicture);
                if (!obj_subpicture)
                    continue;
                status = subpicture_deassociate_1(obj_subpicture, obj_surface);
                if (status == VA_STATUS_SUCCESS)
                    ++n;
            }
            if (n != n_assocs)
                vdpau_error_message("vaDestroySurfaces(): surface 0x%08x still "
                                    "has %d subpictures associated to it\n",
                                    obj_surface->base.id, n_assocs - n);
            free(obj_surface->assocs);
            obj_surface->assocs = NULL;
        }
        obj_surface->assocs_count = 0;
        obj_surface->assocs_count_max = 0;

        object_heap_free(&driver_data->surface_heap, (object_base_p)obj_surface);
    }
    return VA_STATUS_SUCCESS;
}

// vaCreateSurfaces
VAStatus
vdpau_CreateSurfaces(
    VADriverContextP    ctx,
    int                 width,
    int                 height,
    int                 format,
    int                 num_surfaces,
    VASurfaceID         *surfaces
)
{
    VDPAU_DRIVER_DATA_INIT;

    VAStatus va_status = VA_STATUS_SUCCESS;
    VdpVideoSurface vdp_surface = VDP_INVALID_HANDLE;
    VdpChromaType vdp_chroma_type = get_VdpChromaType(format);
    VdpStatus vdp_status;
    int i;

    /* We only support one format */
    if (format != VA_RT_FORMAT_YUV420)
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;

    for (i = 0; i < num_surfaces; i++) {
        vdp_status = vdpau_video_surface_create(driver_data,
                                                driver_data->vdp_device,
                                                vdp_chroma_type,
                                                width, height,
                                                &vdp_surface);
        if (vdp_status != VDP_STATUS_OK) {
            va_status = VA_STATUS_ERROR_ALLOCATION_FAILED;
            break;
        }

        int va_surface = object_heap_allocate(&driver_data->surface_heap);
        object_surface_p obj_surface;
        if ((obj_surface = VDPAU_SURFACE(va_surface)) == NULL) {
            va_status = VA_STATUS_ERROR_ALLOCATION_FAILED;
            break;
        }
        obj_surface->va_context         = VA_INVALID_ID;
        obj_surface->va_surface_status  = VASurfaceReady;
        obj_surface->vdp_surface        = vdp_surface;
        obj_surface->vdp_output_surface = VDP_INVALID_HANDLE;
        obj_surface->width              = width;
        obj_surface->height             = height;
        obj_surface->assocs             = NULL;
        obj_surface->assocs_count       = 0;
        obj_surface->assocs_count_max   = 0;
        obj_surface->vdp_chroma_type    = vdp_chroma_type;
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
VAStatus vdpau_DestroyContext(VADriverContextP ctx, VAContextID context)
{
    VDPAU_DRIVER_DATA_INIT;
    int i;

    object_context_p obj_context = VDPAU_CONTEXT(context);
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
VAStatus
vdpau_CreateContext(
    VADriverContextP    ctx,
    VAConfigID          config_id,
    int                 picture_width,
    int                 picture_height,
    int                 flag,
    VASurfaceID        *render_targets,
    int                 num_render_targets,
    VAContextID        *context
)
{
    VDPAU_DRIVER_DATA_INIT;

    if (context)
        *context = VA_INVALID_ID;

    object_config_p obj_config;
    if ((obj_config = VDPAU_CONFIG(config_id)) == NULL)
        return VA_STATUS_ERROR_INVALID_CONFIG;

    /* XXX: validate flag */

    VdpDecoderProfile vdp_profile;
    uint32_t max_width, max_height;
    int i;
    vdp_profile = get_VdpDecoderProfile(obj_config->profile);
    if (!get_max_surface_size(driver_data, vdp_profile, &max_width, &max_height))
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    if (picture_width > max_width || picture_height > max_height)
        return VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED;

    VAContextID context_id = object_heap_allocate(&driver_data->context_heap);
    if (context_id == VA_INVALID_ID)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;

    object_context_p obj_context = VDPAU_CONTEXT(context_id);
    ASSERT(obj_context);
    if (!obj_context)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    if (context)
        *context = context_id;

    obj_context->context_id             = context_id;
    obj_context->config_id              = config_id;
    obj_context->current_render_target  = VA_INVALID_SURFACE;
    obj_context->picture_width          = picture_width;
    obj_context->picture_height         = picture_height;
    obj_context->num_render_targets     = num_render_targets;
    obj_context->flags                  = flag;
    obj_context->max_ref_frames         = -1;
    obj_context->render_targets         = (VASurfaceID *)
        calloc(num_render_targets, sizeof(VASurfaceID));
    obj_context->dead_buffers           = NULL;
    obj_context->dead_buffers_count     = 0;
    obj_context->dead_buffers_count_max = 0;
    obj_context->vdp_codec              = get_VdpCodec(vdp_profile);
    obj_context->vdp_profile            = vdp_profile;
    obj_context->vdp_decoder            = VDP_INVALID_HANDLE;
    obj_context->vdp_video_surfaces     = (VdpVideoSurface *)
        calloc(num_render_targets, sizeof(VdpVideoSurface));
    obj_context->vdp_bitstream_buffers = NULL;
    obj_context->vdp_bitstream_buffers_count = 0;
    obj_context->vdp_bitstream_buffers_count_max = 0;
    obj_context->ref_frames_count       = 0;
    for (i = 0; i < ARRAY_ELEMS(obj_context->ref_frames); i++)
        obj_context->ref_frames[i]      = VA_INVALID_SURFACE;

    if (obj_context->render_targets == NULL || obj_context->vdp_video_surfaces == NULL) {
        vdpau_DestroyContext(ctx, context_id);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    for (i = 0; i < num_render_targets; i++) {
        object_surface_t *obj_surface;
        if ((obj_surface = VDPAU_SURFACE(render_targets[i])) == NULL) {
            vdpau_DestroyContext(ctx, context_id);
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        obj_context->render_targets[i] = render_targets[i];
        obj_context->vdp_video_surfaces[i] = obj_surface->vdp_surface;
        /* XXX: assume we can only associate a surface to a single context */
        ASSERT(obj_surface->va_context == VA_INVALID_ID);
        obj_surface->va_context = context_id;
    }
    return VA_STATUS_SUCCESS;
}

// Query surface status
VAStatus
query_surface_status(
    vdpau_driver_data_t *driver_data,
    object_surface_p     obj_surface,
    VASurfaceStatus     *status
)
{
    VAStatus va_status = VA_STATUS_SUCCESS;

    if (obj_surface->va_surface_status == VASurfaceDisplaying &&
        obj_surface->vdp_output_surface != VDP_INVALID_HANDLE) {
        object_output_p obj_output = obj_surface->output_surface;
        ASSERT(obj_output);
        if (!obj_output)
            return VA_STATUS_ERROR_INVALID_SURFACE;

        VdpPresentationQueueStatus vdp_queue_status;
        VdpTime vdp_dummy_time;
        VdpStatus vdp_status;
        vdp_status = vdpau_presentation_queue_query_surface_status(
            driver_data,
            obj_output->vdp_flip_queue,
            obj_surface->vdp_output_surface,
            &vdp_queue_status,
            &vdp_dummy_time
        );
        va_status = vdpau_get_VAStatus(driver_data, vdp_status);

        if (vdp_queue_status == VDP_PRESENTATION_QUEUE_STATUS_VISIBLE) {
            obj_surface->va_surface_status  = VASurfaceReady;
            obj_surface->vdp_output_surface = VDP_INVALID_HANDLE;
        }
    }

    if (status)
        *status = obj_surface->va_surface_status;

    return va_status;
}

// vaQuerySurfaceStatus
VAStatus
vdpau_QuerySurfaceStatus(
    VADriverContextP    ctx,
    VASurfaceID         render_target,
    VASurfaceStatus    *status
)
{
    VDPAU_DRIVER_DATA_INIT;

    object_surface_p obj_surface = VDPAU_SURFACE(render_target);
    ASSERT(obj_surface);
    if (obj_surface == NULL)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    return query_surface_status(driver_data, obj_surface, status);
}

// Wait for the surface to complete pending operations
VAStatus
sync_surface(
    vdpau_driver_data_t *driver_data,
    object_surface_p     obj_surface
)
{
    /* VDPAU only supports status interface for in-progress display */
    /* XXX: polling is bad but there currently is no alternative */
    for (;;) {
        VASurfaceStatus va_surface_status;
        VAStatus va_status = query_surface_status(driver_data,
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

// vaSyncSurface
VAStatus
vdpau_SyncSurface2(
    VADriverContextP    ctx,
    VASurfaceID         render_target
)
{
    VDPAU_DRIVER_DATA_INIT;

    object_surface_p obj_surface = VDPAU_SURFACE(render_target);
    ASSERT(obj_surface);
    if (!obj_surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    /* Assume that this shouldn't be called before vaEndPicture() */
    object_context_p obj_context = VDPAU_CONTEXT(obj_surface->va_context);
    if (obj_context) {
        ASSERT(obj_context->current_render_target != obj_surface->base.id);
        if (obj_context->current_render_target == obj_surface->base.id)
            return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    return sync_surface(driver_data, obj_surface);
}

VAStatus
vdpau_SyncSurface3(
    VADriverContextP    ctx,
    VAContextID         context,
    VASurfaceID         render_target
)
{
    VDPAU_DRIVER_DATA_INIT;

    object_surface_p obj_surface = VDPAU_SURFACE(render_target);
    ASSERT(obj_surface);
    if (!obj_surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    /* Assume that this shouldn't be called before vaEndPicture() */
    object_context_p obj_context = VDPAU_CONTEXT(context);
    if (obj_context) {
        ASSERT(obj_context->current_render_target != obj_surface->base.id);
        if (obj_context->current_render_target == obj_surface->base.id)
            return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    return sync_surface(driver_data, obj_surface);
}

// Ensure VA Display Attributes are initialized
static int ensure_display_attributes(vdpau_driver_data_t *driver_data)
{
    VADisplayAttribute *attr;

    if (driver_data->va_display_attrs_count > 0)
        return 0;

    attr = &driver_data->va_display_attrs[0];

    attr->type      = VADisplayAttribDirectSurface;
    attr->value     = 0; /* VdpVideoSurface is copied into VdpOutputSurface */
    attr->min_value = attr->value;
    attr->max_value = attr->value;
    attr->flags     = VA_DISPLAY_ATTRIB_GETTABLE;
    attr++;

    driver_data->va_display_attrs_count = attr - driver_data->va_display_attrs;
    return 0;
}

// Look up for the specified VA display attribute
static VADisplayAttribute *
get_display_attribute(
    vdpau_driver_data_t *driver_data,
    VADisplayAttribType  type
)
{
    if (ensure_display_attributes(driver_data) < 0)
        return NULL;

    unsigned int i;
    for (i = 0; i < driver_data->va_display_attrs_count; i++) {
        if (driver_data->va_display_attrs[i].type == type)
            return &driver_data->va_display_attrs[i];
    }
    return NULL;
}

// vaQueryDisplayAttributes
VAStatus
vdpau_QueryDisplayAttributes(
    VADriverContextP    ctx,
    VADisplayAttribute *attr_list,
    int                *num_attributes
)
{
    VDPAU_DRIVER_DATA_INIT;

    if (ensure_display_attributes(driver_data) < 0)
        return VA_STATUS_ERROR_OPERATION_FAILED;

    if (attr_list)
        memcpy(attr_list, driver_data->va_display_attrs,
               driver_data->va_display_attrs_count * sizeof(attr_list[0]));

    if (num_attributes)
        *num_attributes = driver_data->va_display_attrs_count;

    return VA_STATUS_SUCCESS;
}

// vaGetDisplayAttributes
VAStatus
vdpau_GetDisplayAttributes(
    VADriverContextP    ctx,
    VADisplayAttribute *attr_list,
    int                 num_attributes
)
{
    VDPAU_DRIVER_DATA_INIT;

    unsigned int i;
    for (i = 0; i < num_attributes; i++) {
        VADisplayAttribute * const dst_attr = &attr_list[i];
        VADisplayAttribute *src_attr;

        src_attr = get_display_attribute(driver_data, dst_attr->type);
        if (src_attr && (src_attr->flags & VA_DISPLAY_ATTRIB_GETTABLE) != 0) {
            dst_attr->min_value = src_attr->min_value;
            dst_attr->max_value = src_attr->max_value;
            dst_attr->value     = src_attr->value;
        }
        else
            dst_attr->flags    &= ~VA_DISPLAY_ATTRIB_GETTABLE;
    }
    return VA_STATUS_SUCCESS;
}

// vaSetDisplayAttributes
VAStatus
vdpau_SetDisplayAttributes(
    VADriverContextP    ctx,
    VADisplayAttribute *attr_list,
    int                 num_attributes
)
{
    VDPAU_DRIVER_DATA_INIT;

    unsigned int i;
    for (i = 0; i < num_attributes; i++) {
        VADisplayAttribute * const src_attr = &attr_list[i];
        VADisplayAttribute *dst_attr;

        dst_attr = get_display_attribute(driver_data, src_attr->type);
        if (!dst_attr)
            return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;

        if ((dst_attr->flags & VA_DISPLAY_ATTRIB_SETTABLE) != 0)
            dst_attr->value = src_attr->value;
    }
    return VA_STATUS_SUCCESS;
}

// vaDbgCopySurfaceToBuffer (not a PUBLIC interface)
VAStatus
vdpau_DbgCopySurfaceToBuffer(
    VADriverContextP    ctx,
    VASurfaceID         surface,
    void              **buffer,
    unsigned int       *stride
)
{
    /* TODO */
    return VA_STATUS_ERROR_UNKNOWN;
}

#if VA_CHECK_VERSION(0,30,0)
// vaCreateSurfaceFromCIFrame
VAStatus
vdpau_CreateSurfaceFromCIFrame(
    VADriverContextP    ctx,
    unsigned long       frame_id,
    VASurfaceID        *surface
)
{
    /* TODO */
    return VA_STATUS_ERROR_UNKNOWN;
}

// vaCreateSurfaceFromV4L2Buf
VAStatus
vdpau_CreateSurfaceFromV4L2Buf(
    VADriverContextP    ctx,
    int                 v4l2_fd,
    struct v4l2_format *v4l2_fmt,
    struct v4l2_buffer *v4l2_buf,
    VASurfaceID        *surface
)
{
    /* TODO */
    return VA_STATUS_ERROR_UNKNOWN;
}

// vaCopySurfaceToBuffer
VAStatus
vdpau_CopySurfaceToBuffer(
    VADriverContextP    ctx,
    VASurfaceID         surface,
    unsigned int       *fourcc,
    unsigned int       *luma_stride,
    unsigned int       *chroma_u_stride,
    unsigned int       *chroma_v_stride,
    unsigned int       *luma_offset,
    unsigned int       *chroma_u_offset,
    unsigned int       *chroma_v_offset,
    void              **buffer
)
{
    /* TODO */
    return VA_STATUS_ERROR_UNKNOWN;
}
#endif
