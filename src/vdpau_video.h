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

#include <va/va.h>
#include <vdpau/vdpau.h>
#include <vdpau/vdpau_x11.h>
#include "object_heap.h"

#define VDPAU_MAX_PROFILES		12
#define VDPAU_MAX_ENTRYPOINTS		5
#define VDPAU_MAX_CONFIG_ATTRIBUTES	10
#define VDPAU_MAX_IMAGE_FORMATS		10
#define VDPAU_MAX_SUBPIC_FORMATS	4
#define VDPAU_MAX_DISPLAY_ATTRIBUTES	4
#define VDPAU_MAX_OUTPUT_SURFACES	2
#define VDPAU_STR_DRIVER_VENDOR		"Splitted-Desktop Systems"
#define VDPAU_STR_DRIVER_NAME		"VDPAU backend for VA API"

typedef enum {
    VDP_IMPLEMENTATION_NVIDIA = 1,
} VdpImplementation;

typedef enum {
    VDP_IMAGE_FORMAT_TYPE_YCBCR = 1,
    VDP_IMAGE_FORMAT_TYPE_RGBA,
    VDP_IMAGE_FORMAT_TYPE_INDEXED
} VdpImageFormatType;

typedef enum {
    VDP_CODEC_MPEG1 = 1,
    VDP_CODEC_MPEG2,
    VDP_CODEC_MPEG4,
    VDP_CODEC_H264,
    VDP_CODEC_VC1
} VdpCodec;

typedef struct vdpau_vtable vdpau_vtable_t;
struct vdpau_vtable {
    VdpDeviceDestroy		*vdp_device_destroy;
    VdpVideoSurfaceCreate	*vdp_video_surface_create;
    VdpVideoSurfaceDestroy	*vdp_video_surface_destroy;
    VdpVideoSurfaceGetBitsYCbCr	*vdp_video_surface_get_bits_ycbcr;
    VdpOutputSurfaceCreate	*vdp_output_surface_create;
    VdpOutputSurfaceDestroy	*vdp_output_surface_destroy;
    VdpOutputSurfaceGetBitsNative *vdp_output_surface_get_bits_native;
    VdpVideoMixerCreate		*vdp_video_mixer_create;
    VdpVideoMixerDestroy	*vdp_video_mixer_destroy;
    VdpVideoMixerRender		*vdp_video_mixer_render;
    VdpPresentationQueueCreate	*vdp_presentation_queue_create;
    VdpPresentationQueueDestroy	*vdp_presentation_queue_destroy;
    VdpPresentationQueueDisplay	*vdp_presentation_queue_display;
    VdpPresentationQueueBlockUntilSurfaceIdle *vdp_presentation_queue_block_until_surface_idle;
    VdpPresentationQueueTargetCreateX11	*vdp_presentation_queue_target_create_x11;
    VdpPresentationQueueTargetDestroy	*vdp_presentation_queue_target_destroy;
    VdpDecoderCreate		*vdp_decoder_create;
    VdpDecoderDestroy		*vdp_decoder_destroy;
    VdpDecoderRender		*vdp_decoder_render;
    VdpDecoderQueryCapabilities	*vdp_decoder_query_capabilities;
    VdpVideoSurfaceQueryGetPutBitsYCbCrCapabilities *vdp_video_surface_query_ycbcr_caps;
    VdpOutputSurfaceQueryGetPutBitsNativeCapabilities *vdp_output_surface_query_rgba_caps;
    VdpGetApiVersion		*vdp_get_api_version;
    VdpGetInformationString	*vdp_get_information_string;
    VdpGetErrorString		*vdp_get_error_string;
};

typedef struct vdpau_driver_data vdpau_driver_data_t;
struct vdpau_driver_data {
    void			*va_context;
    struct object_heap		 config_heap;
    struct object_heap		 context_heap;
    struct object_heap		 surface_heap;
    struct object_heap		 buffer_heap;
    struct object_heap		 output_heap;
    struct object_heap		 image_heap;
    VdpDevice			 vdp_device;
    VdpGetProcAddress		*vdp_get_proc_address;
    struct vdpau_vtable		 vdp_vtable;
    VdpChromaType		 vdp_chroma_format; /* XXX: move elsewhere? */
    VdpImplementation		 vdp_impl_type;
    uint32_t			 vdp_impl_version;
};

typedef struct object_config object_config_t;
struct object_config {
    struct object_base		 base;
    VAProfile			 profile;
    VAEntrypoint		 entrypoint;
    VAConfigAttrib		 attrib_list[VDPAU_MAX_CONFIG_ATTRIBUTES];
    int				 attrib_count;
};

typedef struct object_context object_context_t;
struct object_context {
    struct object_base		 base;
    VAContextID			 context_id;
    VAConfigID			 config_id;
    VASurfaceID			 current_render_target;
    int				 picture_width;
    int				 picture_height;
    int				 num_render_targets;
    int				 flags;
    int				 max_ref_frames;
    VASurfaceID			 output_surface;
    VASurfaceID			*render_targets;
    VABufferID			*dead_buffers;
    uint32_t			 dead_buffers_count;
    uint32_t			 dead_buffers_count_max;
    void			*last_slice_params;
    unsigned int		 last_slice_params_count;
    VdpCodec			 vdp_codec;
    VdpDecoderProfile		 vdp_profile;
    VdpDecoder			 vdp_decoder;
    VdpVideoMixer                vdp_video_mixer;
    VdpVideoSurface		*vdp_video_surfaces;
    VdpBitstreamBuffer		*vdp_bitstream_buffers;
    unsigned int		 vdp_bitstream_buffers_count;
    unsigned int		 vdp_bitstream_buffers_count_max;
    union {
	VdpPictureInfoMPEG1Or2	 mpeg2;
	VdpPictureInfoH264	 h264;
	VdpPictureInfoVC1	 vc1;
    }				 vdp_picture_info;
};

typedef struct object_surface object_surface_t;
struct object_surface {
    struct object_base		 base;
    VAContextID			 va_context;
    VdpVideoSurface		 vdp_surface;
    unsigned int		 width;
    unsigned int		 height;
};

typedef struct object_buffer object_buffer_t;
struct object_buffer {
    struct object_base		 base;
    VABufferType		 type;
    void			*buffer_data;
    uint32_t                     buffer_size;
    int				 max_num_elements;
    int				 num_elements;
};

typedef struct object_output object_output_t;
struct object_output {
    struct object_base		 base;
    Drawable			 drawable;
    uint32_t			 width;
    uint32_t			 height;
    VdpPresentationQueue	 vdp_flip_queue;
    VdpPresentationQueueTarget	 vdp_flip_target;
    uint32_t			 output_surface_width;
    uint32_t			 output_surface_height;
    VdpOutputSurface		 vdp_output_surfaces[VDPAU_MAX_OUTPUT_SURFACES];
    int				 current_output_surface;
};

typedef struct object_image object_image_t;
struct object_image {
    struct object_base		 base;
    VAImage			*image;
};

typedef object_config_t		*object_config_p;
typedef object_context_t	*object_context_p;
typedef object_surface_t	*object_surface_p;
typedef object_buffer_t		*object_buffer_p;
typedef object_output_t		*object_output_p;
typedef object_image_t		*object_image_p;

#endif /* VDPAU_VIDEO_H */
