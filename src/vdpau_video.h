/*
 *  vdpau_video.h - VDPAU backend for VA API (shareed data)
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

#ifndef VDPAU_VIDEO_H
#define VDPAU_VIDEO_H

#include "vdpau_driver.h"
#include "vdpau_decode.h"

typedef struct SubpictureAssociation *SubpictureAssociationP;
struct SubpictureAssociation {
    VASubpictureID               subpicture;
    VASurfaceID                  surface;
    VARectangle                  src_rect;
    VARectangle                  dst_rect;
    unsigned int                 flags;
};

typedef struct object_config object_config_t;
struct object_config {
    struct object_base           base;
    VAProfile                    profile;
    VAEntrypoint                 entrypoint;
    VAConfigAttrib               attrib_list[VDPAU_MAX_CONFIG_ATTRIBUTES];
    int                          attrib_count;
};

typedef struct object_context object_context_t;
struct object_context {
    struct object_base           base;
    VAContextID                  context_id;
    VAConfigID                   config_id;
    VASurfaceID                  current_render_target;
    int                          picture_width;
    int                          picture_height;
    int                          num_render_targets;
    int                          flags;
    int                          max_ref_frames;
    VASurfaceID                  ref_frames[16];
    unsigned int                 ref_frames_count;
    VASurfaceID                  output_surface;
    VASurfaceID                 *render_targets;
    VABufferID                  *dead_buffers;
    uint32_t                     dead_buffers_count;
    uint32_t                     dead_buffers_count_max;
    void                        *last_slice_params;
    unsigned int                 last_slice_params_count;
    VdpCodec                     vdp_codec;
    VdpDecoderProfile            vdp_profile;
    VdpDecoder                   vdp_decoder;
    VdpVideoMixer                vdp_video_mixer;
    VdpVideoSurface             *vdp_video_surfaces;
    VdpBitstreamBuffer          *vdp_bitstream_buffers;
    unsigned int                 vdp_bitstream_buffers_count;
    unsigned int                 vdp_bitstream_buffers_count_max;
    union {
        VdpPictureInfoMPEG1Or2   mpeg2;
        VdpPictureInfoH264       h264;
        VdpPictureInfoVC1        vc1;
    }                            vdp_picture_info;
};

typedef struct object_surface object_surface_t;
struct object_surface {
    struct object_base           base;
    VAContextID                  va_context;
    VASurfaceStatus              va_surface_status;
    VdpVideoSurface              vdp_surface;
    VdpOutputSurface             vdp_output_surface;
    unsigned int                 width;
    unsigned int                 height;
    union {
        VdpReferenceFrameH264    h264;
    }                            vdp_ref_frame;
    SubpictureAssociationP      *assocs;
    unsigned int                 assocs_count;
    unsigned int                 assocs_count_max;
};

// Query surface status
VAStatus
query_surface_status(
    vdpau_driver_data_t *driver_data,
    object_context_p     obj_context,
    object_surface_p     obj_surface,
    VASurfaceStatus     *status
) attribute_hidden;

// Wait for the surface to complete pending operations
VAStatus
sync_surface(
    vdpau_driver_data_t *driver_data,
    object_context_p     obj_context,
    object_surface_p     obj_surface
) attribute_hidden;
 
// Add subpicture association to surface
// NOTE: the subpicture owns the SubpictureAssociation object
int surface_add_association(
    object_surface_p            obj_surface,
    SubpictureAssociationP      assoc
) attribute_hidden;

// Remove subpicture association from surface
// NOTE: the subpicture owns the SubpictureAssociation object
int surface_remove_association(
    object_surface_p            obj_surface,
    SubpictureAssociationP      assoc
) attribute_hidden;

// vaGetConfigAttributes
VAStatus
vdpau_GetConfigAttributes(
    VADriverContextP    ctx,
    VAProfile           profile,
    VAEntrypoint        entrypoint,
    VAConfigAttrib     *attrib_list,
    int                 num_attribs
) attribute_hidden;

// vaCreateConfig
VAStatus
vdpau_CreateConfig(
    VADriverContextP    ctx,
    VAProfile           profile,
    VAEntrypoint        entrypoint,
    VAConfigAttrib     *attrib_list,
    int                 num_attribs,
    VAConfigID         *config_id
) attribute_hidden;

// vaDestroyConfig
VAStatus
vdpau_DestroyConfig(
    VADriverContextP    ctx,
    VAConfigID          config_id
) attribute_hidden;

// vaQueryConfigAttributes
VAStatus
vdpau_QueryConfigAttributes(
    VADriverContextP    ctx,
    VAConfigID          config_id,
    VAProfile          *profile,
    VAEntrypoint       *entrypoint,
    VAConfigAttrib     *attrib_list,
    int                *num_attribs
) attribute_hidden;

// vaCreateSurfaces
VAStatus
vdpau_CreateSurfaces(
    VADriverContextP    ctx,
    int                 width,
    int                 height,
    int                 format,
    int                 num_surfaces,
    VASurfaceID        *surfaces
) attribute_hidden;

// vaDestroySurfaces
VAStatus
vdpau_DestroySurfaces(
    VADriverContextP    ctx,
    VASurfaceID        *surface_list,
    int                 num_surfaces
) attribute_hidden;

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
) attribute_hidden;

// vaDestroyContext
VAStatus
vdpau_DestroyContext(
    VADriverContextP    ctx,
    VAContextID         context
) attribute_hidden;

// vaQuerySurfaceStatus
VAStatus
vdpau_QuerySurfaceStatus(
    VADriverContextP    ctx,
    VASurfaceID         render_target,
    VASurfaceStatus    *status
) attribute_hidden;

// vaSyncSurface 2-args variant (>= 0.31)
VAStatus
vdpau_SyncSurface2(
    VADriverContextP    ctx,
    VASurfaceID         render_target
) attribute_hidden;

// vaSyncSurface 3-args variant (<= 0.30)
VAStatus
vdpau_SyncSurface3(
    VADriverContextP    ctx,
    VAContextID         context,
    VASurfaceID         render_target
) attribute_hidden;

// vaQueryDisplayAttributes
VAStatus
vdpau_QueryDisplayAttributes(
    VADriverContextP    ctx,
    VADisplayAttribute *attr_list,
    int                *num_attributes
) attribute_hidden;

// vaGetDisplayAttributes
VAStatus
vdpau_GetDisplayAttributes(
    VADriverContextP    ctx,
    VADisplayAttribute *attr_list,
    int                 num_attributes
) attribute_hidden;

// vaSetDisplayAttributes
VAStatus
vdpau_SetDisplayAttributes(
    VADriverContextP    ctx,
    VADisplayAttribute *attr_list,
    int                 num_attributes
) attribute_hidden;

// vaDbgCopySurfaceToBuffer (not a PUBLIC interface)
VAStatus
vdpau_DbgCopySurfaceToBuffer(
    VADriverContextP    ctx,
    VASurfaceID         surface,
    void              **buffer,
    unsigned int       *stride
) attribute_hidden;

#if VA_CHECK_VERSION(0,30,0)
// vaCreateSurfaceFromCIFrame
VAStatus
vdpau_CreateSurfaceFromCIFrame(
    VADriverContextP    ctx,
    unsigned long       frame_id,
    VASurfaceID        *surface
) attribute_hidden;

// vaCreateSurfaceFromV4L2Buf
VAStatus
vdpau_CreateSurfaceFromV4L2Buf(
    VADriverContextP    ctx,
    int                 v4l2_fd,
    struct v4l2_format *v4l2_fmt,
    struct v4l2_buffer *v4l2_buf,
    VASurfaceID        *surface
) attribute_hidden;

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
) attribute_hidden;
#endif

#endif /* VDPAU_VIDEO_H */
