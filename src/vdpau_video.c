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
#include <va_backend.h>
#include <stdarg.h>

#define DEBUG 1
#include "debug.h"

#define ASSERT assert

#define INIT_DRIVER_DATA \
	struct vdpau_driver_data *driver_data = (struct vdpau_driver_data *)ctx->pDriverData

#define CONFIG(id) \
	((object_config_p)object_heap_lookup(&driver_data->config_heap, id))
#define CONTEXT(id) \
	((object_context_p)object_heap_lookup(&driver_data->context_heap, id))
#define SURFACE(id) \
	((object_surface_p)object_heap_lookup(&driver_data->surface_heap, id))
#define BUFFER(id) \
	((object_buffer_p)object_heap_lookup(&driver_data->buffer_heap, id))
#define OUTPUT(id) \
	((object_output_p)object_heap_lookup(&driver_data->output_heap, id))

#define CONFIG_ID_OFFSET	0x01000000
#define CONTEXT_ID_OFFSET	0x02000000
#define SURFACE_ID_OFFSET	0x04000000
#define BUFFER_ID_OFFSET	0x08000000
#define OUTPUT_ID_OFFSET	0x10000000


/* ====================================================================== */
/* === Helpers                                                        === */
/* ====================================================================== */

// Returns X drawable dimensions
static void get_drawable_size(Display  *display,
			      Drawable  drawable,
			      uint32_t *pwidth,
			      uint32_t *pheight)
{
    Window rootwin;
    int x, y;
    unsigned int width, height, border_width, depth;
    XGetGeometry(display, drawable, &rootwin,
		 &x, &y, &width, &height, &border_width, &depth);
    if (pwidth)
	*pwidth = width;
    if (pheight)
	*pheight = height;
}

// Returns string representation of VABufferType
static const char *string_of_VABufferType(VABufferType type)
{
    const char *str = NULL;
    switch (type) {
#define _(X) case X: str = #X; break
	_(VAPictureParameterBufferType);
	_(VAIQMatrixBufferType);
	_(VABitPlaneBufferType);
	_(VASliceGroupMapBufferType);
	_(VASliceParameterBufferType);
	_(VASliceDataBufferType);
	_(VAMacroblockParameterBufferType);
	_(VAResidualDataBufferType);
	_(VADeblockingParameterBufferType);
	_(VAImageBufferType);
#undef _
    }
    return str;
}

// Translates VdpStatus to VAStatus
static VAStatus get_VAStatus(VdpStatus status)
{
    VAStatus va_status;
    switch (status) {
    case VDP_STATUS_OK:
	va_status = VA_STATUS_SUCCESS;
	break;
    case VDP_STATUS_INVALID_DECODER_PROFILE:
	va_status = VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
	break;
    default:
	D(bug("WARNING: unknown VdpStatus %d\n", status));
	va_status = VA_STATUS_ERROR_UNKNOWN;
	break;
    }
    return va_status;
}

// Determines whether VAPictureH264 is used a reference
static inline int get_VAPictureH264_is_reference(const VAPictureH264 *va_pic)
{
    return (va_pic->flags & (VA_PICTURE_H264_LONG_TERM_REFERENCE|
			     VA_PICTURE_H264_SHORT_TERM_REFERENCE)) != 0;
}

// Returns string representation of VdpCodec
static const char *string_of_VdpCodec(VdpCodec codec)
{
    const char *str = NULL;
    switch (codec) {
#define _(X) case VDP_CODEC_##X: str = #X; break
	_(MPEG1);
	_(MPEG2);
	_(MPEG4);
	_(H264);
	_(VC1);
#undef _
    }
    return str;
}

// Translates VdpDecoderProfile to VdpCodec
static VdpCodec get_VdpCodec(VdpDecoderProfile profile)
{
    switch (profile) {
    case VDP_DECODER_PROFILE_MPEG1:
	return VDP_CODEC_MPEG1;
    case VDP_DECODER_PROFILE_MPEG2_SIMPLE:
    case VDP_DECODER_PROFILE_MPEG2_MAIN:
	return VDP_CODEC_MPEG2;
    case VDP_DECODER_PROFILE_H264_BASELINE:
    case VDP_DECODER_PROFILE_H264_MAIN:
    case VDP_DECODER_PROFILE_H264_HIGH:
	return VDP_CODEC_H264;
    case VDP_DECODER_PROFILE_VC1_SIMPLE:
    case VDP_DECODER_PROFILE_VC1_MAIN:
    case VDP_DECODER_PROFILE_VC1_ADVANCED:
	return VDP_CODEC_VC1;
    }
    ASSERT(profile);
    return 0;
}

// Computes value for VdpDecoderCreate()::max_references parameter
static int get_VdpDecoder_max_references(VdpDecoderProfile profile,
					 uint32_t          width,
					 uint32_t          height)
{
    int max_references = 2;
    switch (profile) {
    case VDP_DECODER_PROFILE_H264_MAIN:
    case VDP_DECODER_PROFILE_H264_HIGH:
    {
	/* level 4.1 limits */
	uint32_t aligned_width  = (width + 15) & -16;
	uint32_t aligned_height = (height + 15) & -16;
	uint32_t surface_size   = (aligned_width * aligned_height * 3) / 2;
	if ((max_references = (12 * 1024 * 1024) / surface_size) > 16)
	    max_references = 16;
	break;
    }
    }
    return max_references;
}

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

// Translates VAProfile to VdpDecoderProfile
static VdpDecoderProfile get_VdpDecoderProfile(VAProfile profile)
{
    switch (profile) {
    case VAProfileMPEG2Simple:	return VDP_DECODER_PROFILE_MPEG2_SIMPLE;
    case VAProfileMPEG2Main:	return VDP_DECODER_PROFILE_MPEG2_MAIN;
    case VAProfileH264Baseline:	return VDP_DECODER_PROFILE_H264_BASELINE;
    case VAProfileH264Main:	return VDP_DECODER_PROFILE_H264_MAIN;
    case VAProfileH264High:	return VDP_DECODER_PROFILE_H264_HIGH;
    case VAProfileVC1Simple:	return VDP_DECODER_PROFILE_VC1_SIMPLE;
    case VAProfileVC1Main:	return VDP_DECODER_PROFILE_VC1_MAIN;
    case VAProfileVC1Advanced:	return VDP_DECODER_PROFILE_VC1_ADVANCED;
    }
    ASSERT(profile);
    return (VdpDecoderProfile)-1;
}


/* ====================================================================== */
/* === VDPAU Gate                                                     === */
/* ====================================================================== */

// VdpVideoSurfaceCreate
static inline VdpStatus
vdpau_video_surface_create(vdpau_driver_data_t *driver_data,
			   VdpDevice            device,
			   VdpChromaType        chroma_type,
			   uint32_t             width,
			   uint32_t             height,
			   VdpVideoSurface     *surface)
{
    if (driver_data == NULL)
	return VDP_STATUS_INVALID_POINTER;
    if (driver_data->vdp_vtable.vdp_video_surface_create == NULL)
	return VDP_STATUS_INVALID_POINTER;
    return driver_data->vdp_vtable.vdp_video_surface_create(device,
							    chroma_type,
							    width,
							    height,
							    surface);
}

// VdpVideoSurfaceDestroy
static inline VdpStatus
vdpau_video_surface_destroy(vdpau_driver_data_t *driver_data,
			    VdpVideoSurface      surface)
{
    if (driver_data == NULL)
	return VDP_STATUS_INVALID_POINTER;
    if (driver_data->vdp_vtable.vdp_video_surface_destroy == NULL)
	return VDP_STATUS_INVALID_POINTER;
    return driver_data->vdp_vtable.vdp_video_surface_destroy(surface);
}

// VdpOutputSurfaceCreate
static inline VdpStatus
vdpau_output_surface_create(vdpau_driver_data_t *driver_data,
			    VdpDevice            device,
			    VdpRGBAFormat        rgba_format,
			    uint32_t             width,
			    uint32_t             height,
			    VdpOutputSurface    *surface)
{
    if (driver_data == NULL)
	return VDP_STATUS_INVALID_POINTER;
    if (driver_data->vdp_vtable.vdp_output_surface_create == NULL)
	return VDP_STATUS_INVALID_POINTER;
    return driver_data->vdp_vtable.vdp_output_surface_create(device,
							     rgba_format,
							     width,
							     height,
							     surface);
}

// VdpOutputSurfaceDestroy
static inline VdpStatus
vdpau_output_surface_destroy(vdpau_driver_data_t *driver_data,
			     VdpOutputSurface     surface)
{
    if (driver_data == NULL)
	return VDP_STATUS_INVALID_POINTER;
    if (driver_data->vdp_vtable.vdp_output_surface_destroy == NULL)
	return VDP_STATUS_INVALID_POINTER;
    return driver_data->vdp_vtable.vdp_output_surface_destroy(surface);
}

// VdpVideoMixerCreate
static inline VdpStatus
vdpau_video_mixer_create(vdpau_driver_data_t          *driver_data,
			 VdpDevice                     device,
			 uint32_t                      feature_count,
			 VdpVideoMixerFeature const   *features,
			 uint32_t                      parameter_count,
			 VdpVideoMixerParameter const *parameters,
			 const void                   *parameter_values,
			 VdpVideoMixer                *mixer)
{
    if (driver_data == NULL)
	return VDP_STATUS_INVALID_POINTER;
    if (driver_data->vdp_vtable.vdp_video_mixer_create == NULL)
	return VDP_STATUS_INVALID_POINTER;
    return driver_data->vdp_vtable.vdp_video_mixer_create(device,
							  feature_count,
							  features,
							  parameter_count,
							  parameters,
							  parameter_values,
							  mixer);
}

// VdpVideoMixerDestroy
static inline VdpStatus
vdpau_video_mixer_destroy(vdpau_driver_data_t *driver_data,
			  VdpVideoMixer        mixer)
{
    if (driver_data == NULL)
	return VDP_STATUS_INVALID_POINTER;
    if (driver_data->vdp_vtable.vdp_video_mixer_destroy == NULL)
	return VDP_STATUS_INVALID_POINTER;
    return driver_data->vdp_vtable.vdp_video_mixer_destroy(mixer);
}

// VdpVideoMixerRender
static inline VdpStatus
vdpau_video_mixer_render(vdpau_driver_data_t          *driver_data,
			 VdpVideoMixer                 mixer,
			 VdpOutputSurface              background_surface,
			 const VdpRect                *background_source_rect,
			 VdpVideoMixerPictureStructure current_picture_structure,
			 uint32_t                      video_surface_past_count,
			 const VdpVideoSurface        *video_surface_past,
			 VdpVideoSurface               video_surface_current,
			 uint32_t                      video_surface_future_count,
			 const VdpVideoSurface        *video_surface_future,
			 const VdpRect                *video_source_rect,
			 VdpOutputSurface              destination_surface,
			 const VdpRect                *destination_rect,
			 const VdpRect                *destination_video_rect,
			 uint32_t                      layer_count,
			 const VdpLayer               *layers)
{
    if (driver_data == NULL)
	return VDP_STATUS_INVALID_POINTER;
    if (driver_data->vdp_vtable.vdp_video_mixer_render == NULL)
	return VDP_STATUS_INVALID_POINTER;
    return driver_data->vdp_vtable.vdp_video_mixer_render(mixer,
							  background_surface,
							  background_source_rect,
							  current_picture_structure,
							  video_surface_past_count,
							  video_surface_past,
							  video_surface_current,
							  video_surface_future_count,
							  video_surface_future,
							  video_source_rect,
							  destination_surface,
							  destination_rect,
							  destination_video_rect,
							  layer_count,
							  layers);
}

// VdpPresentationQueueCreate
static inline VdpStatus
vdpau_presentation_queue_create(vdpau_driver_data_t       *driver_data,
				VdpDevice                  device,
				VdpPresentationQueueTarget presentation_queue_target,
				VdpPresentationQueue      *presentation_queue)
{
    if (driver_data == NULL)
	return VDP_STATUS_INVALID_POINTER;
    if (driver_data->vdp_vtable.vdp_presentation_queue_create == NULL)
	return VDP_STATUS_INVALID_POINTER;
    return driver_data->vdp_vtable.vdp_presentation_queue_create(device,
								 presentation_queue_target,
								 presentation_queue);
}

// VdpPresentationQueueDestroy
static inline VdpStatus
vdpau_presentation_queue_destroy(vdpau_driver_data_t *driver_data,
				 VdpPresentationQueue presentation_queue)
{
    if (driver_data == NULL)
	return VDP_STATUS_INVALID_POINTER;
    if (driver_data->vdp_vtable.vdp_presentation_queue_destroy == NULL)
	return VDP_STATUS_INVALID_POINTER;
    return driver_data->vdp_vtable.vdp_presentation_queue_destroy(presentation_queue);
}

// VdpPresentationQueueDisplay
static inline VdpStatus
vdpau_presentation_queue_display(vdpau_driver_data_t *driver_data,
				 VdpPresentationQueue presentation_queue,
				 VdpOutputSurface     surface,
				 uint32_t             clip_width,
				 uint32_t             clip_height,
				 VdpTime              earliest_presentation_time)
{
    if (driver_data == NULL)
	return VDP_STATUS_INVALID_POINTER;
    if (driver_data->vdp_vtable.vdp_presentation_queue_display == NULL)
	return VDP_STATUS_INVALID_POINTER;
    return driver_data->vdp_vtable.vdp_presentation_queue_display(presentation_queue,
								  surface,
								  clip_width,
								  clip_height,
								  earliest_presentation_time);
}

// VdpPresentationQueueBlockUntilSurfaceIdle
static inline VdpStatus
vdpau_presentation_queue_block_until_surface_idle(vdpau_driver_data_t *driver_data,
						  VdpPresentationQueue presentation_queue,
						  VdpOutputSurface     surface,
						  VdpTime             *first_presentation_time)
{
    if (driver_data == NULL)
	return VDP_STATUS_INVALID_POINTER;
    if (driver_data->vdp_vtable.vdp_presentation_queue_block_until_surface_idle == NULL)
	return VDP_STATUS_INVALID_POINTER;
    return driver_data->vdp_vtable.vdp_presentation_queue_block_until_surface_idle(presentation_queue,
										   surface,
										   first_presentation_time);
}

// VdpPresentationQueueTargetCreateX11
static inline VdpStatus
vdpau_presentation_queue_target_create_x11(vdpau_driver_data_t        *driver_data,
					   VdpDevice                   device,
					   Drawable                    drawable,
					   VdpPresentationQueueTarget *target)
{
    if (driver_data == NULL)
	return VDP_STATUS_INVALID_POINTER;
    if (driver_data->vdp_vtable.vdp_presentation_queue_target_create_x11 == NULL)
	return VDP_STATUS_INVALID_POINTER;
    return driver_data->vdp_vtable.vdp_presentation_queue_target_create_x11(device,
									    drawable,
									    target);
}

// VdpPresentationQueueTargetDestroy
static inline VdpStatus
vdpau_presentation_queue_target_destroy(vdpau_driver_data_t       *driver_data,
					VdpPresentationQueueTarget presentation_queue_target)
{
    if (driver_data == NULL)
	return VDP_STATUS_INVALID_POINTER;
    if (driver_data->vdp_vtable.vdp_presentation_queue_target_destroy == NULL)
	return VDP_STATUS_INVALID_POINTER;
    return driver_data->vdp_vtable.vdp_presentation_queue_target_destroy(presentation_queue_target);
}

// VdpDecoderCreate
static inline VdpStatus
vdpau_decoder_create(vdpau_driver_data_t *driver_data,
		     VdpDevice            device,
		     VdpDecoderProfile    profile,
		     uint32_t             width,
		     uint32_t             height,
		     uint32_t             max_references,
		     VdpDecoder          *decoder)
{
    if (driver_data == NULL)
	return VDP_STATUS_INVALID_POINTER;
    if (driver_data->vdp_vtable.vdp_decoder_create == NULL)
	return VDP_STATUS_INVALID_POINTER;
    return driver_data->vdp_vtable.vdp_decoder_create(device,
						      profile,
						      width,
						      height,
						      max_references,
						      decoder);
}

// VdpDecoderDestroy
static inline VdpStatus
vdpau_decoder_destroy(vdpau_driver_data_t *driver_data,
		      VdpDecoder           decoder)
{
    if (driver_data == NULL)
	return VDP_STATUS_INVALID_POINTER;
    if (driver_data->vdp_vtable.vdp_decoder_destroy == NULL)
	return VDP_STATUS_INVALID_POINTER;
    return driver_data->vdp_vtable.vdp_decoder_destroy(decoder);
}

// VdpDecoderRender
static inline VdpStatus
vdpau_decoder_render(vdpau_driver_data_t      *driver_data,
		     VdpDecoder                decoder,
		     VdpVideoSurface           target,
		     VdpPictureInfo const     *picture_info,
		     uint32_t                  bitstream_buffers_count,
		     VdpBitstreamBuffer const *bitstream_buffers)
		     
{
    if (driver_data == NULL)
	return VDP_STATUS_INVALID_POINTER;
    if (driver_data->vdp_vtable.vdp_decoder_render == NULL)
	return VDP_STATUS_INVALID_POINTER;
    return driver_data->vdp_vtable.vdp_decoder_render(decoder,
						      target,
						      picture_info,
						      bitstream_buffers_count,
						      bitstream_buffers);
}

// VdpDecoderQueryCapabilities
static inline VdpStatus
vdpau_decoder_query_capabilities(vdpau_driver_data_t *driver_data,
				 VdpDevice            device,
				 VdpDecoderProfile    profile,
				 VdpBool             *is_supported,
				 uint32_t            *max_level,
				 uint32_t            *max_references,
				 uint32_t            *max_width,
				 uint32_t            *max_height)
{
    if (driver_data == NULL)
	return VDP_STATUS_INVALID_POINTER;
    if (driver_data->vdp_vtable.vdp_decoder_query_capabilities == NULL)
	return VDP_STATUS_INVALID_POINTER;
    return driver_data->vdp_vtable.vdp_decoder_query_capabilities(device,
								  profile,
								  is_supported,
								  max_level,
								  max_references,
								  max_width,
								  max_height);
}

// Checks whether the VDPAU implementation supports the specified profile
static inline VdpBool
vdpau_is_supported_profile(vdpau_driver_data_t *driver_data,
			   VdpDecoderProfile    profile)
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
    return vdp_status == VDP_STATUS_OK && is_supported;
}

// Returns the maximum dimensions supported by the VDPAU implementation for that profile
static inline VdpBool
vdpau_get_surface_size_max(vdpau_driver_data_t *driver_data,
			   VdpDecoderProfile    profile,
			   uint32_t            *pmax_width,
			   uint32_t            *pmax_height)
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


/* ====================================================================== */
/* === VA API Implementation with VDPAU                               === */
/* ====================================================================== */

static void destroy_output_surface(vdpau_driver_data_t *driver_data, VASurfaceID surface)
{
    if (surface == 0)
	return;

    object_output_p obj_output = OUTPUT(surface);
    ASSERT(obj_output);
    if (obj_output == NULL)
	return;

    if (obj_output->vdp_flip_queue != VDP_INVALID_HANDLE) {
	vdpau_presentation_queue_destroy(driver_data, obj_output->vdp_flip_queue);
	obj_output->vdp_flip_queue = VDP_INVALID_HANDLE;
    }

    if (obj_output->vdp_flip_target != VDP_INVALID_HANDLE) {
	vdpau_presentation_queue_target_destroy(driver_data, obj_output->vdp_flip_target);
	obj_output->vdp_flip_target = VDP_INVALID_HANDLE;
    }

    int i;
    for (i = 0; i < 2; i++) {
	VdpOutputSurface vdp_output_surface = obj_output->vdp_output_surfaces[i];
	if (vdp_output_surface) {
	    vdpau_output_surface_destroy(driver_data, vdp_output_surface);
	    obj_output->vdp_output_surfaces[i] = VDP_INVALID_HANDLE;
	}
    }

    object_heap_free(&driver_data->output_heap, (object_base_p)obj_output);
}

static VASurfaceID create_output_surface(vdpau_driver_data_t *driver_data,
					 uint32_t             width,
					 uint32_t             height)
{
    int surface = object_heap_allocate(&driver_data->output_heap);
    if (surface < 0)
	return 0;

    object_output_p obj_output = OUTPUT(surface);
    ASSERT(obj_output);
    if (obj_output == NULL)
	return 0;

    obj_output->drawable		= None;
    obj_output->width			= 0;
    obj_output->height			= 0;
    obj_output->vdp_flip_queue		= VDP_INVALID_HANDLE;
    obj_output->vdp_flip_target		= VDP_INVALID_HANDLE;
    obj_output->output_surface_width	= width;
    obj_output->output_surface_height	= height;
    obj_output->vdp_output_surfaces[0]	= VDP_INVALID_HANDLE;
    obj_output->vdp_output_surfaces[1]	= VDP_INVALID_HANDLE;
    obj_output->current_output_surface	= 0;

    VADriverContextP const ctx = driver_data->va_context;
    uint32_t display_width, display_height;
    display_width  = DisplayWidth(ctx->x11_dpy, ctx->x11_screen);
    display_height = DisplayHeight(ctx->x11_dpy, ctx->x11_screen);
    if (obj_output->output_surface_width < display_width)
	obj_output->output_surface_width = display_width;
    if (obj_output->output_surface_height < display_height)
	obj_output->output_surface_height = display_height;

    int i;
    for (i = 0; i < 3; i++) {
	VdpStatus vdp_status;
	VdpOutputSurface vdp_output_surface;
	vdp_status = vdpau_output_surface_create(driver_data,
						 driver_data->vdp_device,
						 VDP_RGBA_FORMAT_B8G8R8A8,
						 obj_output->output_surface_width,
						 obj_output->output_surface_height,
						 &vdp_output_surface);

	if (vdp_status != VDP_STATUS_OK) {
	    destroy_output_surface(driver_data, surface);
	    return 0;
	}

	obj_output->vdp_output_surfaces[i] = vdp_output_surface;
    }

    return surface;
}

// vaQueryConfigProfiles
static VAStatus
vdpau_QueryConfigProfiles(VADriverContextP ctx,
			  VAProfile *profile_list,	/* out */
			  int *num_profiles		/* out */)
{
    INIT_DRIVER_DATA;

    static const VAProfile va_profiles[] = {
	VAProfileMPEG2Simple,
	VAProfileMPEG2Main,
	VAProfileH264Baseline,
	VAProfileH264Main,
	VAProfileH264High,
	VAProfileVC1Simple,
	VAProfileVC1Main,
	VAProfileVC1Advanced
    };
    int i, n = 0;
    for (i = 0; i < sizeof(va_profiles)/sizeof(va_profiles[0]); i++) {
	VAProfile profile = va_profiles[i];
	VdpDecoderProfile vdp_profile = get_VdpDecoderProfile(profile);
	if (vdpau_is_supported_profile(driver_data, profile))
	    profile_list[n++] = profile;
    }

    /* If the assert fails then VDPAU_MAX_PROFILES needs to be bigger */
    ASSERT(n <= VDPAU_MAX_PROFILES);
    if (num_profiles)
	*num_profiles = n;

    return VA_STATUS_SUCCESS;
}

// vaQueryConfigEntrypoints
static VAStatus
vdpau_QueryConfigEntrypoints(VADriverContextP ctx,
			     VAProfile profile,
			     VAEntrypoint *entrypoint_list,	/* out */
			     int *num_entrypoints		/* out */)
{
    INIT_DRIVER_DATA;
    VAEntrypoint entrypoint;

    switch (profile) {
    case VAProfileMPEG2Simple:
    case VAProfileMPEG2Main:
	entrypoint = VAEntrypointVLD;
	break;
    case VAProfileH264Baseline:
    case VAProfileH264Main:
    case VAProfileH264High:
	entrypoint = VAEntrypointVLD;
	break;
    case VAProfileVC1Simple:
    case VAProfileVC1Main:
    case VAProfileVC1Advanced:
	entrypoint = VAEntrypointVLD;
	break;
    default:
	entrypoint = 0;
	break;
    }

    if (entrypoint_list)
	*entrypoint_list = entrypoint;
    if (num_entrypoints)
	*num_entrypoints = entrypoint != 0;

    return VA_STATUS_SUCCESS;
}

// vaGetConfigAttributes
static VAStatus
vdpau_GetConfigAttributes(VADriverContextP ctx,
			  VAProfile profile,
			  VAEntrypoint entrypoint,
			  VAConfigAttrib *attrib_list,	/* in/out */
			  int num_attribs)
{
    INIT_DRIVER_DATA;
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
static VAStatus
vdpau_DestroyConfig(VADriverContextP ctx, VAConfigID config_id)
{
    INIT_DRIVER_DATA;

    VAStatus va_status;
    object_config_p obj_config;

    if ((obj_config = CONFIG(config_id)) == NULL)
        return VA_STATUS_ERROR_INVALID_CONFIG;

    object_heap_free(&driver_data->config_heap, (object_base_p)obj_config);
    return VA_STATUS_SUCCESS;
}

// vaCreateConfig
static VAStatus
vdpau_CreateConfig(VADriverContextP ctx,
		   VAProfile profile,
		   VAEntrypoint entrypoint,
		   VAConfigAttrib *attrib_list,
		   int num_attribs,
		   VAConfigID *config_id)		/* out */
{
    INIT_DRIVER_DATA;

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
    if ((obj_config = CONFIG(configID)) == NULL)
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
static VAStatus
vdpau_QueryConfigAttributes(VADriverContextP ctx,
			    VAConfigID config_id,
			    VAProfile *profile,		/* out */
			    VAEntrypoint *entrypoint, 	/* out */
			    VAConfigAttrib *attrib_list,/* out */
			    int *num_attribs)		/* out */
{
    INIT_DRIVER_DATA;

    VAStatus va_status = VA_STATUS_SUCCESS;
    object_config_p obj_config;
    int i;

    obj_config = CONFIG(config_id);
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

// vaDestroySurfaces
static VAStatus
vdpau_DestroySurfaces(VADriverContextP ctx,
		      VASurfaceID *surface_list,
		      int num_surfaces)
{
    INIT_DRIVER_DATA;

    int i;
    for (i = num_surfaces - 1; i >= 0; i--) {
	object_surface_p obj_surface = SURFACE(surface_list[i]);
	ASSERT(obj_surface);
	if (obj_surface->vdp_surface != VDP_INVALID_HANDLE) {
	    vdpau_video_surface_destroy(driver_data, obj_surface->vdp_surface);
	    obj_surface->vdp_surface = VDP_INVALID_HANDLE;
	}
	object_heap_free(&driver_data->surface_heap, (object_base_p)obj_surface);
    }
    return VA_STATUS_SUCCESS;
}

// vaCreateSurfaces
static VAStatus
vdpau_CreateSurfaces(VADriverContextP ctx,
		     int width,
		     int height,
		     int format,
		     int num_surfaces,
		     VASurfaceID *surfaces)		/* out */
{
    INIT_DRIVER_DATA;

    VAStatus va_status = VA_STATUS_SUCCESS;
    VdpVideoSurface vdp_surface = VDP_INVALID_HANDLE;
    VdpStatus vdp_status;
    int i;

    /* We only support one format */
    if (format != VA_RT_FORMAT_YUV420)
	return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    driver_data->vdp_chroma_format = get_VdpChromaType(format);

    for (i = 0; i < num_surfaces; i++) {
	vdp_status = vdpau_video_surface_create(driver_data,
						driver_data->vdp_device,
						driver_data->vdp_chroma_format,
						width, height,
						&vdp_surface);
	if (vdp_status != VDP_STATUS_OK) {
	    va_status = VA_STATUS_ERROR_ALLOCATION_FAILED;
	    break;
	}

        int va_surface = object_heap_allocate(&driver_data->surface_heap);
	object_surface_p obj_surface;
	if ((obj_surface = SURFACE(va_surface)) == NULL) {
            va_status = VA_STATUS_ERROR_ALLOCATION_FAILED;
            break;
        }
	obj_surface->va_context = 0;
	obj_surface->vdp_surface = vdp_surface;
        surfaces[i] = va_surface;
	vdp_surface = VDP_INVALID_HANDLE;
    }

    /* Error recovery */
    if (va_status != VA_STATUS_SUCCESS) {
	if (vdp_surface != VDP_INVALID_HANDLE)
	    vdpau_video_surface_destroy(driver_data, vdp_surface);
	vdpau_DestroySurfaces(ctx, surfaces, i);
    }

    return va_status;
}

// vaQueryImageFormats
static VAStatus
vdpau_QueryImageFormats(VADriverContextP ctx,
			VAImageFormat *format_list,	/* out */
			int *num_formats)		/* out */
{
    INIT_DRIVER_DATA;

    /* TODO */
    return VA_STATUS_SUCCESS;
}

// vaCreateImage
static VAStatus
vdpau_CreateImage(VADriverContextP ctx,
		  VAImageFormat *format,
		  int width,
		  int height,
		  VAImage *image)			/* out */
{
    INIT_DRIVER_DATA;

    /* TODO */
    return VA_STATUS_SUCCESS;
}

// vaDeriveImage
static VAStatus
vdpau_DeriveImage(VADriverContextP ctx,
		  VASurfaceID surface,
		  VAImage *image)			/* out */
{
    INIT_DRIVER_DATA;

    /* TODO */
    return VA_STATUS_SUCCESS;
}

// vaDestroyImage
static VAStatus
vdpau_DestroyImage(VADriverContextP ctx,
		   VAImageID image)
{
    INIT_DRIVER_DATA;

    /* TODO */
    return VA_STATUS_SUCCESS;
}

// vaSetImagePalette
static VAStatus
vdpau_SetImagePalette(VADriverContextP ctx,
		      VAImageID image,
		      unsigned char *palette)
{
    INIT_DRIVER_DATA;

    /* TODO */
    return VA_STATUS_SUCCESS;
}

// vaGetImage
static VAStatus
vdpau_GetImage(VADriverContextP ctx,
	       VASurfaceID surface,
	       int x,     /* coordinates of the upper left source pixel */
	       int y,
	       unsigned int width, /* width and height of the region */
	       unsigned int height,
	       VAImageID image)
{
    INIT_DRIVER_DATA;

    /* TODO */
    return VA_STATUS_SUCCESS;
}

// vaPutImage
static VAStatus
vdpau_PutImage(VADriverContextP ctx,
	       VASurfaceID surface,
	       VAImageID image,
	       int src_x,
	       int src_y,
	       unsigned int width,
	       unsigned int height,
	       int dest_x,
	       int dest_y)
{
    INIT_DRIVER_DATA;

    /* TODO */
    return VA_STATUS_SUCCESS;
}

// vaPutImage2
static VAStatus
vdpau_PutImage2(VADriverContextP ctx,
		VASurfaceID surface,
		VAImageID image,
		int src_x,
		int src_y,
		unsigned int src_width,
		unsigned int src_height,
		int dest_x,
		int dest_y,
		unsigned int dest_width,
		unsigned int dest_height)
{
    INIT_DRIVER_DATA;

    /* TODO */
    return VA_STATUS_SUCCESS;
}

// vaQuerySubpictureFormats
static VAStatus
vdpau_QuerySubpictureFormats(VADriverContextP ctx,
			     VAImageFormat *format_list,/* out */
			     unsigned int *flags,	/* out */
			     unsigned int *num_formats)	/* out */
{
    INIT_DRIVER_DATA;

    /* TODO */
    return VA_STATUS_SUCCESS;
}

// vaCreateSubpicture
static VAStatus
vdpau_CreateSubpicture(VADriverContextP ctx,
		       VAImageID image,
		       VASubpictureID *subpicture)	/* out */
{
    INIT_DRIVER_DATA;

    /* TODO */
    return VA_STATUS_SUCCESS;
}

// vaDestroySubpicture
static VAStatus
vdpau_DestroySubpicture(VADriverContextP ctx,
			VASubpictureID subpicture)
{
    INIT_DRIVER_DATA;

    /* TODO */
    return VA_STATUS_SUCCESS;
}

// vaSetSubpictureImage
static VAStatus
vdpau_SetSubpictureImage(VADriverContextP ctx,
			 VASubpictureID subpicture,
			 VAImageID image)
{
    INIT_DRIVER_DATA;

    /* TODO */
    return VA_STATUS_SUCCESS;
}

// vaSetSubpicturePalette (not a PUBLIC interface)
static VAStatus
vdpau_SetSubpicturePalette(VADriverContextP ctx,
			   VASubpictureID subpicture,
			   unsigned char *palette)
{
    INIT_DRIVER_DATA;

    /* TODO */
    return VA_STATUS_SUCCESS;
}

// vaSetSubpictureChromaKey
static VAStatus
vdpau_SetSubpictureChromakey(VADriverContextP ctx,
			     VASubpictureID subpicture,
			     unsigned int chromakey_min,
			     unsigned int chromakey_max,
			     unsigned int chromakey_mask)
{
    INIT_DRIVER_DATA;

    /* TODO */
    return VA_STATUS_SUCCESS;
}

// vaSetSubpictureGlobalAlpha
static VAStatus
vdpau_SetSubpictureGlobalAlpha(VADriverContextP ctx,
			       VASubpictureID subpicture,
			       float global_alpha)
{
    INIT_DRIVER_DATA;

    /* TODO */
    return VA_STATUS_SUCCESS;
}

// vaAssociateSubpicture
static VAStatus
vdpau_AssociateSubpicture(VADriverContextP ctx,
			  VASubpictureID subpicture,
			  VASurfaceID *target_surfaces,
			  int num_surfaces,
			  short src_x, /* upper left offset in subpicture */
			  short src_y,
			  short dest_x, /* upper left offset in surface */
			  short dest_y,
			  unsigned short width,
			  unsigned short height,
			  unsigned int flags)
{
    INIT_DRIVER_DATA;

    /* TODO */
    return VA_STATUS_SUCCESS;
}

// vaAssociateSubpicture2
static VAStatus
vdpau_AssociateSubpicture2(VADriverContextP ctx,
			   VASubpictureID subpicture,
			   VASurfaceID *target_surfaces,
			   int num_surfaces,
			   short src_x, /* upper left offset in subpicture */
			   short src_y,
			   unsigned short src_width,
			   unsigned short src_height,
			   short dest_x, /* upper left offset in surface */
			   short dest_y,
			   unsigned short dest_width,
			   unsigned short dest_height,
			   unsigned int flags)
{
    INIT_DRIVER_DATA;

    /* TODO */
    return VA_STATUS_SUCCESS;
}

// vaDeassociateSubpicture
static VAStatus
vdpau_DeassociateSubpicture(VADriverContextP ctx,
			    VASubpictureID subpicture,
			    VASurfaceID *target_surfaces,
			    int num_surfaces)
{
    INIT_DRIVER_DATA;

    /* TODO */
    return VA_STATUS_SUCCESS;
}

// vaDestroyContext
static VAStatus
vdpau_DestroyContext(VADriverContextP ctx,
		     VAContextID context)
{
    INIT_DRIVER_DATA;
    int i;

    object_context_p obj_context = CONTEXT(context);
    ASSERT(obj_context);
    if (obj_context == NULL)
	return VA_STATUS_ERROR_INVALID_CONTEXT;

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

    if (obj_context->vdp_video_mixer != VDP_INVALID_HANDLE) {
	vdpau_video_mixer_destroy(driver_data, obj_context->vdp_video_mixer);
	obj_context->vdp_video_mixer = VDP_INVALID_HANDLE;
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

    if (obj_context->output_surface) {
	destroy_output_surface(driver_data, obj_context->output_surface);
	obj_context->output_surface = 0;
    }

    obj_context->context_id = -1;
    obj_context->config_id = -1;
    obj_context->current_render_target = -1;
    obj_context->picture_width = 0;
    obj_context->picture_height = 0;
    obj_context->num_render_targets = 0;
    obj_context->flags = 0;
    obj_context->dead_buffers_count = 0;
    obj_context->dead_buffers_count_max = 0;

    object_heap_free(&driver_data->context_heap, (object_base_p)obj_context);
    return VA_STATUS_SUCCESS;
}

// vaCreateContext
static VAStatus
vdpau_CreateContext(VADriverContextP ctx,
		    VAConfigID config_id,
		    int picture_width,
		    int picture_height,
		    int flag,
		    VASurfaceID *render_targets,
		    int num_render_targets,
		    VAContextID *context)		/* out */
{
    INIT_DRIVER_DATA;

    if (context)
	*context = 0;

    object_config_p obj_config;
    if ((obj_config = CONFIG(config_id)) == NULL)
        return VA_STATUS_ERROR_INVALID_CONFIG;

    /* XXX: validate flag */

    VdpDecoderProfile vdp_profile;
    uint32_t max_width, max_height;
    vdp_profile = get_VdpDecoderProfile(obj_config->profile);
    if (!vdpau_get_surface_size_max(driver_data, vdp_profile, &max_width, &max_height))
	return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    if (picture_width > max_width || picture_height > max_height)
	return VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED;

    int contextID = object_heap_allocate(&driver_data->context_heap);
    object_context_p obj_context;
    if ((obj_context = CONTEXT(contextID)) == NULL)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    if (context)
	*context = contextID;

    obj_context->context_id		= contextID;
    obj_context->config_id		= config_id;
    obj_context->current_render_target	= -1;
    obj_context->picture_width		= picture_width;
    obj_context->picture_height		= picture_height;
    obj_context->num_render_targets	= num_render_targets;
    obj_context->flags			= flag;
    obj_context->output_surface		=
	create_output_surface(driver_data, picture_width, picture_height);
    obj_context->render_targets		= (VASurfaceID *)
	calloc(num_render_targets, sizeof(VASurfaceID));
    obj_context->dead_buffers		= NULL;
    obj_context->dead_buffers_count	= 0;
    obj_context->dead_buffers_count_max	= 0;
    obj_context->vdp_codec		= get_VdpCodec(vdp_profile);
    obj_context->vdp_profile		= vdp_profile;
    obj_context->vdp_decoder		= VDP_INVALID_HANDLE;
    obj_context->vdp_video_mixer	= VDP_INVALID_HANDLE;
    obj_context->vdp_video_surfaces	= (VdpVideoSurface *)
	calloc(num_render_targets, sizeof(VdpVideoSurface));

    if (obj_context->output_surface == 0) {
	vdpau_DestroyContext(ctx, contextID);
	return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    if (obj_context->render_targets == NULL || obj_context->vdp_video_surfaces == NULL) {
	vdpau_DestroyContext(ctx, contextID);
	return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    int i;
    for (i = 0; i < num_render_targets; i++) {
	object_surface_t *obj_surface;
	if ((obj_surface = SURFACE(render_targets[i])) == NULL) {
	    vdpau_DestroyContext(ctx, contextID);
	    return VA_STATUS_ERROR_INVALID_SURFACE;
	}
	obj_context->render_targets[i] = render_targets[i];
	obj_context->vdp_video_surfaces[i] = obj_surface->vdp_surface;
	/* XXX: assume we can only associate a surface to a single context */
	ASSERT(obj_surface->va_context == 0);
	obj_surface->va_context = contextID;
    }

    VdpStatus vdp_status;
    int vdp_max_references;
    vdp_max_references = get_VdpDecoder_max_references(vdp_profile,
						       picture_width,
						       picture_height);
    vdp_status = vdpau_decoder_create(driver_data,
				      driver_data->vdp_device,
				      vdp_profile,
				      picture_width,
				      picture_height,
				      vdp_max_references,
				      &obj_context->vdp_decoder);
    if (vdp_status != VDP_STATUS_OK) {
	vdpau_DestroyContext(ctx, contextID);
	return get_VAStatus(vdp_status);
    }

    VdpVideoMixerParameter params[] = {
	VDP_VIDEO_MIXER_PARAMETER_VIDEO_SURFACE_WIDTH,
	VDP_VIDEO_MIXER_PARAMETER_VIDEO_SURFACE_HEIGHT,
	VDP_VIDEO_MIXER_PARAMETER_CHROMA_TYPE
    };

    const void *param_values[3];
    param_values[0] = &picture_width;
    param_values[1] = &picture_height;
    param_values[2] = &driver_data->vdp_chroma_format;

    vdp_status = vdpau_video_mixer_create(driver_data,
					  driver_data->vdp_device,
					  0,
					  NULL,
					  sizeof(params) / sizeof(params[0]),
					  params,
					  param_values,
					  &obj_context->vdp_video_mixer);
    if (vdp_status != VDP_STATUS_OK) {
	vdpau_DestroyContext(ctx, contextID);
	return get_VAStatus(vdp_status);
    }

    return VA_STATUS_SUCCESS;
}

static inline int
vdpau_allocate_buffer(object_buffer_p obj_buffer, int size)
{
    obj_buffer->buffer_data = realloc(obj_buffer->buffer_data, size);
    return obj_buffer->buffer_data != NULL;
}

static void
vdpau_destroy_buffer(vdpau_driver_data_t *driver_data,
		     object_buffer_p      obj_buffer)
{
    if (obj_buffer->buffer_data) {
	free(obj_buffer->buffer_data);
	obj_buffer->buffer_data = NULL;
    }
    object_heap_free(&driver_data->buffer_heap, (object_base_p)obj_buffer);
}

static void
vdpau_schedule_destroy_buffer(object_context_p obj_context,
			      object_buffer_p  obj_buffer)
{
    if (obj_context->dead_buffers_count >= obj_context->dead_buffers_count_max) {
	obj_context->dead_buffers_count_max += 4;
	obj_context->dead_buffers = realloc(obj_context->dead_buffers,
					    obj_context->dead_buffers_count_max * sizeof(obj_context->dead_buffers[0]));
	ASSERT(obj_context->dead_buffers);
    }
    ASSERT(obj_context->dead_buffers);
    obj_context->dead_buffers[obj_context->dead_buffers_count] = obj_buffer->base.id;
    obj_context->dead_buffers_count++;
}

static int
vdpau_translate_VASurfaceID(vdpau_driver_data_t *driver_data,
			    VASurfaceID          va_surface,
			    VdpVideoSurface     *vdp_surface)
{
    if (va_surface == 0xffffffff) {
	*vdp_surface = VDP_INVALID_HANDLE;
	return 1;
    }
    object_surface_p obj_surface = SURFACE(va_surface);
    ASSERT(obj_surface);
    if (obj_surface == NULL)
	return 0;
    *vdp_surface = obj_surface->vdp_surface;
    return 1;
}

static int
vdpau_translate_nothing(vdpau_driver_data_t *driver_data,
			object_context_p     obj_context,
			object_buffer_p      obj_buffer)
{
    return 1;
}

static int
vdpau_translate_VASliceDataBuffer(vdpau_driver_data_t *driver_data,
				  object_context_p     obj_context,
				  object_buffer_p      obj_buffer)
{
    VdpBitstreamBuffer * const vdp_bitstream_buffer = &obj_context->vdp_bitstream_buffer;
    vdp_bitstream_buffer->struct_version  = VDP_BITSTREAM_BUFFER_VERSION;
    vdp_bitstream_buffer->bitstream	  = obj_buffer->buffer_data;
    vdp_bitstream_buffer->bitstream_bytes = obj_buffer->buffer_size;
    return 1;
}

static int
vdpau_translate_VAPictureParameterBufferMPEG2(vdpau_driver_data_t *driver_data,
					      object_context_p     obj_context,
					      object_buffer_p      obj_buffer)
{
    VdpPictureInfoMPEG1Or2 * const pinfo = &obj_context->vdp_picture_info.mpeg2;
    VAPictureParameterBufferMPEG2 * const pic_param = obj_buffer->buffer_data;
    if (!vdpau_translate_VASurfaceID(driver_data,
				     pic_param->forward_reference_picture,
				     &pinfo->forward_reference))
	return 0;
    if (!vdpau_translate_VASurfaceID(driver_data,
				     pic_param->backward_reference_picture,
				     &pinfo->backward_reference))
	return 0;
    pinfo->picture_structure		= pic_param->picture_structure;
    pinfo->picture_coding_type		= pic_param->picture_coding_type;
    pinfo->intra_dc_precision		= pic_param->intra_dc_precision;
    pinfo->frame_pred_frame_dct		= pic_param->frame_pred_frame_dct;
    pinfo->concealment_motion_vectors	= pic_param->concealment_motion_vectors;
    pinfo->intra_vlc_format		= pic_param->intra_vlc_format;
    pinfo->alternate_scan		= pic_param->alternate_scan;
    pinfo->q_scale_type			= pic_param->q_scale_type;
    pinfo->top_field_first		= pic_param->top_field_first;
    pinfo->full_pel_forward_vector	= 0;
    pinfo->full_pel_backward_vector	= 0;
    pinfo->f_code[0][0]			= (pic_param->f_code >> 12) & 0xf;
    pinfo->f_code[0][1]			= (pic_param->f_code >>  8) & 0xf;
    pinfo->f_code[1][0]			= (pic_param->f_code >>  4) & 0xf;
    pinfo->f_code[1][1]			= pic_param->f_code & 0xf;
    return 1;
}

static const uint8_t ff_identity[64] = {
    0,   1,  2,  3,  4,  5,  6,  7,
    8,   9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23,
    24, 25, 26, 27, 28, 29, 30, 31,
    32, 33, 34, 35, 36, 37, 38, 39,
    40, 41, 42, 43, 44, 45, 46, 47,
    48, 49, 50, 51, 52, 53, 54, 55,
    56, 57, 58, 59, 60, 61, 62, 63
};

static const uint8_t ff_zigzag_direct[64] = {
    0,   1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

static const uint8_t ff_mpeg1_default_intra_matrix[64] = {
     8, 16, 19, 22, 26, 27, 29, 34,
    16, 16, 22, 24, 27, 29, 34, 37,
    19, 22, 26, 27, 29, 34, 34, 38,
    22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48,
    26, 27, 29, 32, 35, 40, 48, 58,
    26, 27, 29, 34, 38, 46, 56, 69,
    27, 29, 35, 38, 46, 56, 69, 83
};

static const uint8_t ff_mpeg1_default_non_intra_matrix[64] = {
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16
};

static int
vdpau_translate_VAIQMatrixBufferMPEG2(vdpau_driver_data_t *driver_data,
				      object_context_p     obj_context,
				      object_buffer_p      obj_buffer)
{
    VdpPictureInfoMPEG1Or2 * const pinfo = &obj_context->vdp_picture_info.mpeg2;
    VAIQMatrixBufferMPEG2 * const iq_matrix = obj_buffer->buffer_data;

    const uint8_t *intra_matrix;
    const uint8_t *intra_matrix_lookup;
    const uint8_t *inter_matrix;
    const uint8_t *inter_matrix_lookup;
    if (iq_matrix->load_intra_quantiser_matrix) {
	intra_matrix = iq_matrix->intra_quantiser_matrix;
	intra_matrix_lookup = ff_zigzag_direct;
    }
    else {
	intra_matrix = ff_mpeg1_default_intra_matrix;
	intra_matrix_lookup = ff_identity;
    }
    if (iq_matrix->load_non_intra_quantiser_matrix) {
	inter_matrix = iq_matrix->non_intra_quantiser_matrix;
	inter_matrix_lookup = ff_zigzag_direct;
    }
    else {
	inter_matrix = ff_mpeg1_default_non_intra_matrix;
	inter_matrix_lookup = ff_identity;
    }

    int i;
    for (i = 0; i < 64; i++) {
	pinfo->intra_quantizer_matrix[intra_matrix_lookup[i]] =
	    intra_matrix[i];
	pinfo->non_intra_quantizer_matrix[inter_matrix_lookup[i]] =
	    inter_matrix[i];
    }
    return 1;
}

static int
vdpau_translate_VASliceParameterBufferMPEG2(vdpau_driver_data_t *driver_data,
					    object_context_p     obj_context,
					    object_buffer_p      obj_buffer)
{
    VdpPictureInfoMPEG1Or2 * const pinfo = &obj_context->vdp_picture_info.mpeg2;
    pinfo->slice_count = obj_buffer->num_elements;
    return 1;
}

static int
vdpau_translate_VAPictureH264(vdpau_driver_data_t   *driver_data,
			      const VAPictureH264   *va_pic,
			      VdpReferenceFrameH264 *rf)
{
    // Handle invalid surfaces specifically
    if (va_pic->picture_id == 0xffffffff) {
	rf->surface		= VDP_INVALID_HANDLE;
	rf->is_long_term	= VDP_FALSE;
	rf->top_is_reference	= VDP_FALSE;
	rf->bottom_is_reference	= VDP_FALSE;
	rf->field_order_cnt[0]	= 0;
	rf->field_order_cnt[1]	= 0;
	rf->frame_idx		= 0;
	return 1;
    }

    if (!vdpau_translate_VASurfaceID(driver_data, va_pic->picture_id, &rf->surface))
	return 0;
    rf->is_long_term		= (va_pic->flags & VA_PICTURE_H264_LONG_TERM_REFERENCE) != 0;
    if ((va_pic->flags & (VA_PICTURE_H264_TOP_FIELD|VA_PICTURE_H264_BOTTOM_FIELD)) == 0) {
	rf->top_is_reference	= VDP_TRUE;
	rf->bottom_is_reference	= VDP_TRUE;
    }
    else {
	rf->top_is_reference	= (va_pic->flags & VA_PICTURE_H264_TOP_FIELD) != 0;
	rf->bottom_is_reference	= (va_pic->flags & VA_PICTURE_H264_BOTTOM_FIELD) != 0;
    }
    rf->field_order_cnt[0]	= va_pic->TopFieldOrderCnt;
    rf->field_order_cnt[1]	= va_pic->BottomFieldOrderCnt;
    rf->frame_idx		= va_pic->frame_idx;
    return 1;
}

static int
vdpau_translate_VAPictureParameterBufferH264(vdpau_driver_data_t *driver_data,
					     object_context_p     obj_context,
					     object_buffer_p      obj_buffer)
{
    VdpPictureInfoH264 * const pinfo = &obj_context->vdp_picture_info.h264;
    VAPictureParameterBufferH264 * const pic_param = obj_buffer->buffer_data;
    VAPictureH264 * const CurrPic = &pic_param->CurrPic;
    int i;

    pinfo->field_order_cnt[0]		= CurrPic->TopFieldOrderCnt;
    pinfo->field_order_cnt[1]		= CurrPic->BottomFieldOrderCnt;
    pinfo->is_reference			= get_VAPictureH264_is_reference(CurrPic);

    pinfo->frame_num			= pic_param->frame_num;
    pinfo->field_pic_flag		= pic_param->field_pic_flag;
    pinfo->bottom_field_flag		= pic_param->field_pic_flag && (CurrPic->flags & VA_PICTURE_H264_BOTTOM_FIELD) != 0;
    pinfo->num_ref_frames		= pic_param->num_ref_frames;
    pinfo->mb_adaptive_frame_field_flag	= pic_param->mb_adaptive_frame_field_flag;
    pinfo->constrained_intra_pred_flag	= pic_param->constrained_intra_pred_flag;
    pinfo->weighted_pred_flag		= pic_param->weighted_pred_flag;
    pinfo->weighted_bipred_idc		= pic_param->weighted_bipred_idc;
    pinfo->frame_mbs_only_flag		= pic_param->frame_mbs_only_flag;
    pinfo->transform_8x8_mode_flag	= pic_param->transform_8x8_mode_flag;
    pinfo->chroma_qp_index_offset	= pic_param->chroma_qp_index_offset;
    pinfo->second_chroma_qp_index_offset= pic_param->second_chroma_qp_index_offset;
    pinfo->pic_init_qp_minus26		= pic_param->pic_init_qp_minus26;
    pinfo->log2_max_frame_num_minus4	= pic_param->log2_max_frame_num_minus4;
    pinfo->pic_order_cnt_type		= pic_param->pic_order_cnt_type;
    pinfo->log2_max_pic_order_cnt_lsb_minus4 =
	pic_param->log2_max_pic_order_cnt_lsb_minus4;
    pinfo->delta_pic_order_always_zero_flag =
	pic_param->delta_pic_order_always_zero_flag;
    pinfo->direct_8x8_inference_flag	= pic_param->direct_8x8_inference_flag;
    pinfo->entropy_coding_mode_flag	= pic_param->entropy_coding_mode_flag;
    pinfo->pic_order_present_flag	= pic_param->pic_order_present_flag;
    pinfo->deblocking_filter_control_present_flag =
	pic_param->deblocking_filter_control_present_flag;
    pinfo->redundant_pic_cnt_present_flag =
	pic_param->redundant_pic_cnt_present_flag;
    for (i = 0; i < 16; i++) {
	if (!vdpau_translate_VAPictureH264(driver_data,
					   &pic_param->ReferenceFrames[i],
					   &pinfo->referenceFrames[i]))
	    return 0;
    }
    return 1;
}

static int
vdpau_translate_VAIQMatrixBufferH264(vdpau_driver_data_t *driver_data,
				      object_context_p     obj_context,
				      object_buffer_p      obj_buffer)
{
    VdpPictureInfoH264 * const pinfo = &obj_context->vdp_picture_info.h264;
    VAIQMatrixBufferH264 * const iq_matrix = obj_buffer->buffer_data;
    int i, j;

    if (sizeof(pinfo->scaling_lists_4x4) == sizeof(iq_matrix->ScalingList4x4))
	memcpy(pinfo->scaling_lists_4x4, iq_matrix->ScalingList4x4,
	       sizeof(pinfo->scaling_lists_4x4));
    else {
	for (j = 0; j < 6; j++) {
	    for (i = 0; i < 16; i++)
		pinfo->scaling_lists_4x4[j][i] = iq_matrix->ScalingList4x4[j][i];
	}
    }

    if (sizeof(pinfo->scaling_lists_8x8) == sizeof(iq_matrix->ScalingList8x8))
	memcpy(pinfo->scaling_lists_8x8, iq_matrix->ScalingList8x8,
	       sizeof(pinfo->scaling_lists_8x8));
    else {
	for (j = 0; j < 2; j++) {
	    for (i = 0; i < 64; i++)
		pinfo->scaling_lists_8x8[j][i] = iq_matrix->ScalingList8x8[j][i];
	}
    }
    return 1;
}

static int
vdpau_translate_VASliceParameterBufferH264(vdpau_driver_data_t *driver_data,
					    object_context_p     obj_context,
					    object_buffer_p      obj_buffer)
{
    VdpPictureInfoH264 * const pinfo = &obj_context->vdp_picture_info.h264;
    VASliceParameterBufferH264 * const slice_params = obj_buffer->buffer_data;
    VASliceParameterBufferH264 * const slice_param = &slice_params[obj_buffer->num_elements - 1];

    pinfo->slice_count			= obj_buffer->num_elements;
    pinfo->num_ref_idx_l0_active_minus1	= slice_param->num_ref_idx_l0_active_minus1;
    pinfo->num_ref_idx_l1_active_minus1	= slice_param->num_ref_idx_l1_active_minus1;
    return 1;
}

static int
vdpau_translate_VAPictureParameterBufferVC1(vdpau_driver_data_t *driver_data,
					    object_context_p     obj_context,
					    object_buffer_p      obj_buffer)
{
    VdpPictureInfoVC1 * const pinfo = &obj_context->vdp_picture_info.vc1;
    VAPictureParameterBufferVC1 * const pic_param = obj_buffer->buffer_data;
    int picture_type;

    if (!vdpau_translate_VASurfaceID(driver_data,
				     pic_param->forward_reference_picture,
				     &pinfo->forward_reference))
	return 0;
    if (!vdpau_translate_VASurfaceID(driver_data,
				     pic_param->backward_reference_picture,
				     &pinfo->backward_reference))
	return 0;

    if (obj_context->vdp_profile == VDP_DECODER_PROFILE_VC1_ADVANCED) {
	switch (pic_param->picture_type) {
	case 0: picture_type = 0; break; /* I */
	case 1: picture_type = 1; break; /* P */
	case 2: picture_type = 3; break; /* B */
	case 3: picture_type = 4; break; /* BI */
	default: ASSERT(!pic_param->picture_type); return 0;
	}
    }
    else {
	if (pic_param->max_b_frames == 0) {
	    switch (pic_param->picture_type) {
	    case 0: picture_type = 0; break; /* I */
	    case 1: picture_type = 1; break; /* P */
	    default: ASSERT(!pic_param->picture_type); return 0;
	    }
	}
	else {
	    switch (pic_param->picture_type) {
	    case 0: picture_type = 1; break; /* P */
	    case 1: picture_type = 0; break; /* I */
	    case 2: picture_type = 3; break; /* XXX: B or BI */
	    default: ASSERT(!pic_param->picture_type); return 0;
	    }
	}
    }

    pinfo->picture_type		= picture_type;
    pinfo->frame_coding_mode	= pic_param->frame_coding_mode;
    pinfo->postprocflag		= pic_param->post_processing != 0;
    pinfo->pulldown		= pic_param->pulldown;
    pinfo->interlace		= pic_param->interlace;
    pinfo->tfcntrflag		= pic_param->frame_counter_flag;
    pinfo->finterpflag		= pic_param->frame_interpolation_flag;
    pinfo->psf			= pic_param->progressive_segment_frame;
    pinfo->dquant		= pic_param->dquant;
    pinfo->panscan_flag		= pic_param->panscan_flag;
    pinfo->refdist_flag		= pic_param->reference_distance_flag;
    pinfo->quantizer		= pic_param->quantizer;
    pinfo->extended_mv		= pic_param->extended_mv_flag;
    pinfo->extended_dmv		= pic_param->extended_dmv_flag;
    pinfo->overlap		= pic_param->overlap;
    pinfo->vstransform		= pic_param->variable_sized_transform_flag;
    pinfo->loopfilter		= pic_param->loopfilter;
    pinfo->fastuvmc		= pic_param->fast_uvmc_flag;
    pinfo->range_mapy_flag	= pic_param->range_mapping_luma_flag;
    pinfo->range_mapy		= pic_param->range_mapping_luma;
    pinfo->range_mapuv_flag	= pic_param->range_mapping_chroma_flag;
    pinfo->range_mapuv		= pic_param->range_mapping_chroma;
    pinfo->multires		= pic_param->multires;
    pinfo->syncmarker		= pic_param->syncmarker;
    pinfo->rangered		= pic_param->rangered;
    pinfo->maxbframes		= pic_param->max_b_frames;
    pinfo->deblockEnable	= 0; /* XXX: fill */
    pinfo->pquant		= 0; /* XXX: fill */
    return 1;
}

static int
vdpau_translate_VASliceParameterBufferVC1(vdpau_driver_data_t *driver_data,
					  object_context_p     obj_context,
					  object_buffer_p      obj_buffer)
{
    VdpPictureInfoVC1 * const pinfo = &obj_context->vdp_picture_info.vc1;
    VASliceParameterBufferVC1 * const slice_params = obj_buffer->buffer_data;
    VASliceParameterBufferVC1 * const slice_param = &slice_params[obj_buffer->num_elements - 1];

    pinfo->slice_count			= obj_buffer->num_elements;
    return 1;
}

typedef int (*vdpau_translate_buffer_func_t)(vdpau_driver_data_t *driver_data,
					     object_context_p     obj_context,
					     object_buffer_p      obj_buffer);

typedef struct vdpau_translate_buffer_info vdpau_translate_buffer_info_t;
struct vdpau_translate_buffer_info {
    VdpCodec codec;
    VABufferType type;
    vdpau_translate_buffer_func_t func;
};

static int
vdpau_translate_buffer(vdpau_driver_data_t *driver_data,
		       object_context_p     obj_context,
		       object_buffer_p      obj_buffer)
{
    static const vdpau_translate_buffer_info_t translate_info[] = {
#define _(CODEC, TYPE)					\
	{ VDP_CODEC_##CODEC, VA##TYPE##BufferType,	\
	  vdpau_translate_VA##TYPE##Buffer##CODEC }
	_(MPEG2, PictureParameter),
	_(MPEG2, IQMatrix),
	_(MPEG2, SliceParameter),
	_(H264, PictureParameter),
	_(H264, IQMatrix),
	_(H264, SliceParameter),
	_(VC1, PictureParameter),
	_(VC1, SliceParameter),
#undef _
	{ VDP_CODEC_VC1, VABitPlaneBufferType, vdpau_translate_nothing },
	{ 0, VASliceDataBufferType, vdpau_translate_VASliceDataBuffer },
	{ 0, 0, NULL }
    };
    const vdpau_translate_buffer_info_t *tbip;
    for (tbip = translate_info; tbip->func != NULL; tbip++) {
	if (tbip->codec && tbip->codec != obj_context->vdp_codec)
	    continue;
	if (tbip->type != obj_buffer->type)
	    continue;
	return tbip->func(driver_data, obj_context, obj_buffer);
    }
    D(bug("ERROR: no translate function found for %s%s\n",
	  string_of_VABufferType(obj_buffer->type),
	  obj_context->vdp_codec ? string_of_VdpCodec(obj_context->vdp_codec) : NULL));
    return 0;
}

// vaDestroyBuffer
static VAStatus
vdpau_DestroyBuffer(VADriverContextP ctx,
		    VABufferID buffer_id)
{
    INIT_DRIVER_DATA;

    object_buffer_p obj_buffer = BUFFER(buffer_id);
#if 1
    if (obj_buffer)
#else
    ASSERT(obj_buffer);
    if (obj_buffer == NULL)
	return VA_STATUS_ERROR_INVALID_BUFFER;
#endif

    vdpau_destroy_buffer(driver_data, obj_buffer);
    return VA_STATUS_SUCCESS;
}

// vaCreateBuffer
static VAStatus
vdpau_CreateBuffer(VADriverContextP ctx,
		   VAContextID context,		/* in */
		   VABufferType type,		/* in */
		   unsigned int size,		/* in */
		   unsigned int num_elements,	/* in */
		   void *data,			/* in */
		   VABufferID *buf_id)		/* out */
{
    INIT_DRIVER_DATA;

    /* Validate type */
    switch (type) {
    case VAPictureParameterBufferType:
    case VAIQMatrixBufferType:
    case VASliceParameterBufferType:
    case VASliceDataBufferType:
	/* Ok */
	break;
    default:
	D(bug("ERROR: unsupported buffer type %d\n", type));
	return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
    }

    int bufferID = object_heap_allocate(&driver_data->buffer_heap);
    object_buffer_p obj_buffer;
    if ((obj_buffer = BUFFER(bufferID)) == NULL)
	return VA_STATUS_ERROR_ALLOCATION_FAILED;

    obj_buffer->buffer_data = NULL;
    obj_buffer->buffer_size = size * num_elements;
    if (!vdpau_allocate_buffer(obj_buffer, obj_buffer->buffer_size)) {
	vdpau_DestroyBuffer(ctx, bufferID);
	return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    obj_buffer->type = type;
    obj_buffer->max_num_elements = num_elements;
    obj_buffer->num_elements = num_elements;
    if (data)
	memcpy(obj_buffer->buffer_data, data, obj_buffer->buffer_size);

    if (buf_id)
	*buf_id = bufferID;

    return VA_STATUS_SUCCESS;
}

// vaBufferSetNumElements
static VAStatus
vdpau_BufferSetNumElements(VADriverContextP ctx,
			   VABufferID buf_id,		/* in */
			   unsigned int num_elements)	/* in */
{
    INIT_DRIVER_DATA;

    object_buffer_p obj_buffer = BUFFER(buf_id);
    ASSERT(obj_buffer);
    if (obj_buffer == NULL)
	return VA_STATUS_ERROR_INVALID_BUFFER;

    if (num_elements < 0 || num_elements > obj_buffer->max_num_elements)
	return VA_STATUS_ERROR_UNKNOWN;

    obj_buffer->num_elements = num_elements;
    return VA_STATUS_SUCCESS;
}

// vaMapBuffer
static VAStatus
vdpau_MapBuffer(VADriverContextP ctx,
		VABufferID buf_id,		/* in */
		void **pbuf)			/* out */
{
    INIT_DRIVER_DATA;

    object_buffer_p obj_buffer = BUFFER(buf_id);
    ASSERT(obj_buffer);
    if (obj_buffer == NULL)
	return VA_STATUS_ERROR_INVALID_BUFFER;

    if (pbuf)
	*pbuf = obj_buffer->buffer_data;

    if (obj_buffer->buffer_data == NULL)
	return VA_STATUS_ERROR_UNKNOWN;

    return VA_STATUS_SUCCESS;
}

// vaUnmapBuffer
static VAStatus
vdpau_UnmapBuffer(VADriverContextP ctx,
		  VABufferID buf_id)		/* in */
{
    /* Don't do anything there, translate structure in vaRenderPicture() */
    return VA_STATUS_SUCCESS;
}

// vaBeginPicture
static VAStatus
vdpau_BeginPicture(VADriverContextP ctx,
		   VAContextID context,
		   VASurfaceID render_target)
{
    INIT_DRIVER_DATA;

    object_context_p obj_context = CONTEXT(context);
    ASSERT(obj_context);
    if (obj_context == NULL)
	return VA_STATUS_ERROR_INVALID_CONTEXT;

    object_surface_p obj_surface = SURFACE(render_target);
    ASSERT(obj_surface);
    if (obj_surface == NULL)
	return VA_STATUS_ERROR_INVALID_SURFACE;

    obj_context->current_render_target = obj_surface->base.id;
    return VA_STATUS_SUCCESS;
}

// vaRenderPicture
static VAStatus
vdpau_RenderPicture(VADriverContextP ctx,
		    VAContextID context,
		    VABufferID *buffers,
		    int num_buffers)
{
    INIT_DRIVER_DATA;
    int i;

    object_context_p obj_context = CONTEXT(context);
    ASSERT(obj_context);
    if (obj_context == NULL)
	return VA_STATUS_ERROR_INVALID_CONTEXT;

    object_surface_p obj_surface = SURFACE(obj_context->current_render_target);
    ASSERT(obj_surface);
    if (obj_surface == NULL)
	return VA_STATUS_ERROR_INVALID_SURFACE;

    /* Verify that we got valid buffer references */
    for (i = 0; i < num_buffers; i++) {
	object_buffer_p obj_buffer = BUFFER(buffers[i]);
        ASSERT(obj_buffer);
	if (obj_buffer == NULL)
	    return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    /* Translate buffers */
    for (i = 0; i < num_buffers; i++) {
	object_buffer_p obj_buffer = BUFFER(buffers[i]);
	ASSERT(obj_buffer);
	if (!vdpau_translate_buffer(driver_data, obj_context, obj_buffer))
	    return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
	/* Release any buffer that is not VASliceDataBuffer */
	if (obj_buffer->type == VASliceDataBufferType)
	    vdpau_schedule_destroy_buffer(obj_context, obj_buffer);
	else
	    vdpau_destroy_buffer(driver_data, obj_buffer);
    }

    return VA_STATUS_SUCCESS;
}

// vaEndPicture
static VAStatus
vdpau_EndPicture(VADriverContextP ctx,
		 VAContextID context)
{
    INIT_DRIVER_DATA;

    object_context_p obj_context = CONTEXT(context);
    ASSERT(obj_context);
    if (obj_context == NULL)
	return VA_STATUS_ERROR_INVALID_CONTEXT;

    object_surface_p obj_surface = SURFACE(obj_context->current_render_target);
    ASSERT(obj_surface);
    if (obj_surface == NULL)
	return VA_STATUS_ERROR_INVALID_SURFACE;

    object_output_p obj_output = OUTPUT(obj_context->output_surface);
    ASSERT(obj_output);
    if (obj_output == NULL)
	return VA_STATUS_ERROR_INVALID_SURFACE;

    VAStatus va_status;
    VdpStatus vdp_status;
    vdp_status = vdpau_decoder_render(driver_data,
				      obj_context->vdp_decoder,
				      obj_surface->vdp_surface,
				      (VdpPictureInfo)&obj_context->vdp_picture_info,
				      1,
				      &obj_context->vdp_bitstream_buffer);
    va_status = get_VAStatus(vdp_status);

    /* XXX: assume we are done with rendering right away */
    obj_context->current_render_target = -1;

    /* Release pending buffers */
    if (obj_context->dead_buffers_count > 0) {
	ASSERT(obj_context->dead_buffers);
	int i;
	for (i = 0; i < obj_context->dead_buffers_count; i++) {
	    object_buffer_p obj_buffer = BUFFER(obj_context->dead_buffers[i]);
	    ASSERT(obj_buffer);
	    vdpau_destroy_buffer(driver_data, obj_buffer);
	}
	obj_context->dead_buffers_count = 0;
    }

    return va_status;
}

// vaSyncSurface
static VAStatus
vdpau_SyncSurface(VADriverContextP ctx,
		  VAContextID context,
		  VASurfaceID render_target)
{
    INIT_DRIVER_DATA;

    object_context_p obj_context = CONTEXT(context);
    ASSERT(obj_context);
    if (obj_context == NULL)
	return VA_STATUS_ERROR_INVALID_CONTEXT;

    object_surface_p obj_surface = SURFACE(render_target);
    ASSERT(obj_surface);
    if (obj_surface == NULL)
	return VA_STATUS_ERROR_INVALID_SURFACE;

    /* Assume that this shouldn't be called before vaEndPicture() */
    ASSERT(obj_context->current_render_target != obj_surface->base.id);

    return VA_STATUS_SUCCESS;
}

// vaQuerySurfaceStatus
static VAStatus
vdpau_QuerySurfaceStatus(VADriverContextP ctx,
			 VASurfaceID render_target,
			 VASurfaceStatus *status)	/* out */
{
    INIT_DRIVER_DATA;
    VAStatus va_status = VA_STATUS_SUCCESS;
    object_surface_p obj_surface;

    obj_surface = SURFACE(render_target);
    ASSERT(obj_surface);

    *status = VASurfaceReady;

    return va_status;
}

// vaPutSurface
static VAStatus
vdpau_PutSurface(VADriverContextP ctx,
		 VASurfaceID surface,
		 Drawable draw,			/* X Drawable */
		 short srcx,
		 short srcy,
		 unsigned short srcw,
		 unsigned short srch,
		 short destx,
		 short desty,
		 unsigned short destw,
		 unsigned short desth,
		 VARectangle *cliprects,	/* client supplied clip list */
		 unsigned int number_cliprects,	/* number of clip rects in the clip list */
		 unsigned int flags)		/* de-interlacing flags */
{
    INIT_DRIVER_DATA;

    object_surface_p obj_surface = SURFACE(surface);
    ASSERT(obj_surface);
    if (obj_surface == NULL)
	return VA_STATUS_ERROR_INVALID_SURFACE;

    object_context_p obj_context = CONTEXT(obj_surface->va_context);
    ASSERT(obj_context);
    if (obj_context == NULL)
	return VA_STATUS_ERROR_INVALID_CONTEXT;

    object_output_p obj_output = OUTPUT(obj_context->output_surface);
    ASSERT(obj_output);
    if (obj_output == NULL)
	return VA_STATUS_ERROR_INVALID_SURFACE;

    /* XXX: no clip rects supported */
    if (cliprects || number_cliprects > 0)
	return VA_STATUS_ERROR_INVALID_PARAMETER;

    /* XXX: only support VA_FRAME_PICTURE */
    if (flags)
	return VA_STATUS_ERROR_FLAG_NOT_SUPPORTED;

    VdpStatus vdp_status;
    uint32_t width, height;
    get_drawable_size(ctx->x11_dpy, draw, &width, &height);

    if (obj_output->drawable == None) {
	obj_output->drawable	= draw;
	obj_output->width	= width;
	obj_output->height	= height;

	vdp_status = vdpau_presentation_queue_target_create_x11(driver_data,
								driver_data->vdp_device,
								draw,
								&obj_output->vdp_flip_target);

	if (vdp_status != VDP_STATUS_OK)
	    return get_VAStatus(vdp_status);

	vdp_status = vdpau_presentation_queue_create(driver_data,
						     driver_data->vdp_device,
						     obj_output->vdp_flip_target,
						     &obj_output->vdp_flip_queue);
	if (vdp_status != VDP_STATUS_OK)
	    return get_VAStatus(vdp_status);
    }
    ASSERT(obj_output->drawable == draw);
    ASSERT(obj_output->vdp_flip_queue != VDP_INVALID_HANDLE);
    ASSERT(obj_output->vdp_flip_target != VDP_INVALID_HANDLE);

    VdpOutputSurface vdp_output_surface;
    vdp_output_surface = obj_output->vdp_output_surfaces[obj_output->current_output_surface];

    VdpTime dummy_time;
    vdp_status = vdpau_presentation_queue_block_until_surface_idle(driver_data,
								   obj_output->vdp_flip_queue,
								   vdp_output_surface,
								   &dummy_time);
    if (vdp_status != VDP_STATUS_OK)
	return get_VAStatus(vdp_status);

    VdpRect source_rect, output_rect, display_rect;
    source_rect.x0  = srcx;
    source_rect.y0  = srcy;
    source_rect.x1  = source_rect.x0 + srcw;
    source_rect.y1  = source_rect.y0 + srch;
    output_rect.x0  = 0;
    output_rect.y0  = 0;
    output_rect.x1  = obj_output->output_surface_width;
    output_rect.y1  = obj_output->output_surface_height;
    display_rect.x0 = destx;
    display_rect.y0 = desty;
    display_rect.x1 = display_rect.x0 + destw;
    display_rect.y1 = display_rect.y0 + desth;
    vdp_status = vdpau_video_mixer_render(driver_data,
					  obj_context->vdp_video_mixer,
					  VDP_INVALID_HANDLE,
					  NULL,
					  VDP_VIDEO_MIXER_PICTURE_STRUCTURE_FRAME,
					  0, NULL,
					  obj_surface->vdp_surface,
					  0, NULL,
					  &source_rect,
					  vdp_output_surface,
					  &output_rect,
					  &display_rect,
					  0, NULL);

    if (vdp_status != VDP_STATUS_OK)
	return get_VAStatus(vdp_status);

    uint32_t clip_width, clip_height;
    clip_width  = MIN(obj_output->output_surface_width, MAX(output_rect.x1, display_rect.x1));
    clip_height = MIN(obj_output->output_surface_height, MAX(output_rect.y1, display_rect.y1));
    vdp_status = vdpau_presentation_queue_display(driver_data,
						  obj_output->vdp_flip_queue,
						  vdp_output_surface,
						  clip_width,
						  clip_height,
						  0);

    if (vdp_status != VDP_STATUS_OK)
	return get_VAStatus(vdp_status);

    obj_output->current_output_surface ^= 1;

    return VA_STATUS_SUCCESS;
}

// vaQueryDisplayAttributes
static VAStatus
vdpau_QueryDisplayAttributes(VADriverContextP ctx,
			     VADisplayAttribute *attr_list,	/* out */
			     int *num_attributes)		/* out */
{
    /* TODO */
    return VA_STATUS_ERROR_UNKNOWN;
}

// vaGetDisplayAttributes
static VAStatus
vdpau_GetDisplayAttributes(VADriverContextP ctx,
			   VADisplayAttribute *attr_list,	/* in/out */
			   int num_attributes)
{
    /* TODO */
    return VA_STATUS_ERROR_UNKNOWN;
}

// vaSetDisplayAttributes
static VAStatus
vdpau_SetDisplayAttributes(VADriverContextP ctx,
			   VADisplayAttribute *attr_list,
			   int num_attributes)
{
    /* TODO */
    return VA_STATUS_ERROR_UNKNOWN;
}

// vaDbgCopySurfaceToBuffer (not a PUBLIC interface)
static VAStatus
vdpau_DbgCopySurfaceToBuffer(VADriverContextP ctx,
			     VASurfaceID surface,
			     void **buffer,		/* out */
			     unsigned int *stride)	/* out */
{
    /* TODO */
    return VA_STATUS_ERROR_UNKNOWN;
}

// vaTerminate
static VAStatus
vdpau_Terminate(VADriverContextP ctx)
{
    INIT_DRIVER_DATA;
    object_buffer_p obj_buffer;
    object_surface_p obj_surface;
    object_context_p obj_context;
    object_config_p obj_config;
    object_heap_iterator iter;

    /* Clean up left over buffers */
    obj_buffer = (object_buffer_p)object_heap_first(&driver_data->buffer_heap, &iter);
    while (obj_buffer)
    {
        vdpau_information_message("vaTerminate: bufferID %08x still allocated, destroying\n", obj_buffer->base.id);
        vdpau_destroy_buffer(driver_data, obj_buffer);
        obj_buffer = (object_buffer_p)object_heap_next(&driver_data->buffer_heap, &iter);
    }
    object_heap_destroy(&driver_data->buffer_heap);

    /* TODO cleanup */
    object_heap_destroy(&driver_data->output_heap);

    /* TODO cleanup */
    object_heap_destroy(&driver_data->surface_heap);

    /* TODO cleanup */
    object_heap_destroy(&driver_data->context_heap);

    /* Clean up configIDs */
    obj_config = (object_config_p)object_heap_first(&driver_data->config_heap, &iter);
    while (obj_config)
    {
        object_heap_free(&driver_data->config_heap, (object_base_p)obj_config);
        obj_config = (object_config_p)object_heap_next(&driver_data->config_heap, &iter);
    }
    object_heap_destroy(&driver_data->config_heap);

    free(ctx->pDriverData);
    ctx->pDriverData = NULL;

    return VA_STATUS_SUCCESS;
}

#define VDPAU_TERMINATE(CTX, STATUS) \
	vdpau_Terminate_with_status(CTX, VA_STATUS_##STATUS)

static inline VAStatus vdpau_Terminate_with_status(VADriverContextP ctx, VAStatus status)
{
    vdpau_Terminate(ctx);
    return status;
}

// vaInitialize
static VAStatus
vdpau_Initialize(VADriverContextP ctx)
{
    struct vdpau_driver_data *driver_data;
    static char vendor[256] = {0, };
    object_base_p obj;
    int i, result;
    VdpStatus vdp_status;

    if (vendor[0] == '\0')
	sprintf(vendor, "%s %s - %d.%d.%d",
		VDPAU_STR_DRIVER_VENDOR,
		VDPAU_STR_DRIVER_NAME,
		VDPAU_VIDEO_MAJOR_VERSION,
		VDPAU_VIDEO_MINOR_VERSION,
		VDPAU_VIDEO_MICRO_VERSION);

    ctx->version_major		= VA_MAJOR_VERSION;
    ctx->version_minor		= VA_MINOR_VERSION;
    ctx->max_profiles		= VDPAU_MAX_PROFILES;
    ctx->max_entrypoints	= VDPAU_MAX_ENTRYPOINTS;
    ctx->max_attributes		= VDPAU_MAX_CONFIG_ATTRIBUTES;
    ctx->max_image_formats	= VDPAU_MAX_IMAGE_FORMATS;
    ctx->max_subpic_formats	= VDPAU_MAX_SUBPIC_FORMATS;
    ctx->max_display_attributes	= VDPAU_MAX_DISPLAY_ATTRIBUTES;
    ctx->str_vendor		= vendor;

    ctx->vtable.vaTerminate			= vdpau_Terminate;
    ctx->vtable.vaQueryConfigEntrypoints	= vdpau_QueryConfigEntrypoints;
    ctx->vtable.vaQueryConfigProfiles		= vdpau_QueryConfigProfiles;
    ctx->vtable.vaQueryConfigEntrypoints	= vdpau_QueryConfigEntrypoints;
    ctx->vtable.vaQueryConfigAttributes		= vdpau_QueryConfigAttributes;
    ctx->vtable.vaCreateConfig			= vdpau_CreateConfig;
    ctx->vtable.vaDestroyConfig			= vdpau_DestroyConfig;
    ctx->vtable.vaGetConfigAttributes		= vdpau_GetConfigAttributes;
    ctx->vtable.vaCreateSurfaces		= vdpau_CreateSurfaces;
    ctx->vtable.vaDestroySurfaces		= vdpau_DestroySurfaces;
    ctx->vtable.vaCreateContext			= vdpau_CreateContext;
    ctx->vtable.vaDestroyContext		= vdpau_DestroyContext;
    ctx->vtable.vaCreateBuffer			= vdpau_CreateBuffer;
    ctx->vtable.vaBufferSetNumElements		= vdpau_BufferSetNumElements;
    ctx->vtable.vaMapBuffer			= vdpau_MapBuffer;
    ctx->vtable.vaUnmapBuffer			= vdpau_UnmapBuffer;
    ctx->vtable.vaDestroyBuffer			= vdpau_DestroyBuffer;
    ctx->vtable.vaBeginPicture			= vdpau_BeginPicture;
    ctx->vtable.vaRenderPicture			= vdpau_RenderPicture;
    ctx->vtable.vaEndPicture			= vdpau_EndPicture;
    ctx->vtable.vaSyncSurface			= vdpau_SyncSurface;
    ctx->vtable.vaQuerySurfaceStatus		= vdpau_QuerySurfaceStatus;
    ctx->vtable.vaPutSurface			= vdpau_PutSurface;
    ctx->vtable.vaQueryImageFormats		= vdpau_QueryImageFormats;
    ctx->vtable.vaCreateImage			= vdpau_CreateImage;
    ctx->vtable.vaDeriveImage			= vdpau_DeriveImage;
    ctx->vtable.vaDestroyImage			= vdpau_DestroyImage;
    ctx->vtable.vaSetImagePalette		= vdpau_SetImagePalette;
    ctx->vtable.vaGetImage			= vdpau_GetImage;
    ctx->vtable.vaPutImage			= vdpau_PutImage;
    ctx->vtable.vaPutImage2			= vdpau_PutImage2;
    ctx->vtable.vaQuerySubpictureFormats	= vdpau_QuerySubpictureFormats;
    ctx->vtable.vaCreateSubpicture		= vdpau_CreateSubpicture;
    ctx->vtable.vaDestroySubpicture		= vdpau_DestroySubpicture;
    ctx->vtable.vaSetSubpictureImage		= vdpau_SetSubpictureImage;
    ctx->vtable.vaSetSubpicturePalette		= vdpau_SetSubpicturePalette;
    ctx->vtable.vaSetSubpictureChromakey	= vdpau_SetSubpictureChromakey;
    ctx->vtable.vaSetSubpictureGlobalAlpha	= vdpau_SetSubpictureGlobalAlpha;
    ctx->vtable.vaAssociateSubpicture		= vdpau_AssociateSubpicture;
    ctx->vtable.vaAssociateSubpicture2		= vdpau_AssociateSubpicture2;
    ctx->vtable.vaDeassociateSubpicture		= vdpau_DeassociateSubpicture;
    ctx->vtable.vaQueryDisplayAttributes	= vdpau_QueryDisplayAttributes;
    ctx->vtable.vaGetDisplayAttributes		= vdpau_GetDisplayAttributes;
    ctx->vtable.vaSetDisplayAttributes		= vdpau_SetDisplayAttributes;
    ctx->vtable.vaDbgCopySurfaceToBuffer	= vdpau_DbgCopySurfaceToBuffer;

    driver_data = (struct vdpau_driver_data *)calloc(1, sizeof(*driver_data));
    if (driver_data == NULL)
	return VDPAU_TERMINATE(ctx, ERROR_ALLOCATION_FAILED);
    ctx->pDriverData = (void *)driver_data;
    driver_data->va_context = ctx;

    vdp_status = vdp_device_create_x11(ctx->x11_dpy, ctx->x11_screen,
				       &driver_data->vdp_device,
				       &driver_data->vdp_get_proc_address);
    ASSERT(vdp_status == VDP_STATUS_OK);
    if (vdp_status != VDP_STATUS_OK)
	return VDPAU_TERMINATE(ctx, ERROR_UNKNOWN);

#define VDP_INIT_PROC(FUNC_ID, FUNC) do {			\
	vdp_status = driver_data->vdp_get_proc_address		\
	    (driver_data->vdp_device,				\
	     VDP_FUNC_ID_##FUNC_ID,				\
	     (void *)&driver_data->vdp_vtable.vdp_##FUNC);	\
	ASSERT(vdp_status == VDP_STATUS_OK);			\
	if (vdp_status != VDP_STATUS_OK)			\
	    return VDPAU_TERMINATE(ctx, ERROR_UNKNOWN);		\
    } while (0)

    VDP_INIT_PROC(DEVICE_DESTROY,		device_destroy);
    VDP_INIT_PROC(VIDEO_SURFACE_CREATE,		video_surface_create);
    VDP_INIT_PROC(VIDEO_SURFACE_DESTROY,	video_surface_destroy);
    VDP_INIT_PROC(OUTPUT_SURFACE_CREATE,	output_surface_create);
    VDP_INIT_PROC(OUTPUT_SURFACE_DESTROY,	output_surface_destroy);
    VDP_INIT_PROC(VIDEO_MIXER_CREATE,		video_mixer_create);
    VDP_INIT_PROC(VIDEO_MIXER_DESTROY,		video_mixer_destroy);
    VDP_INIT_PROC(VIDEO_MIXER_RENDER,		video_mixer_render);
    VDP_INIT_PROC(PRESENTATION_QUEUE_CREATE,	presentation_queue_create);
    VDP_INIT_PROC(PRESENTATION_QUEUE_DESTROY,	presentation_queue_destroy);
    VDP_INIT_PROC(PRESENTATION_QUEUE_DISPLAY,	presentation_queue_display);
    VDP_INIT_PROC(PRESENTATION_QUEUE_BLOCK_UNTIL_SURFACE_IDLE,
		  presentation_queue_block_until_surface_idle);
    VDP_INIT_PROC(PRESENTATION_QUEUE_TARGET_CREATE_X11,
		  presentation_queue_target_create_x11);
    VDP_INIT_PROC(PRESENTATION_QUEUE_TARGET_DESTROY,
		  presentation_queue_target_destroy);
    VDP_INIT_PROC(DECODER_CREATE,		decoder_create);
    VDP_INIT_PROC(DECODER_DESTROY,		decoder_destroy);
    VDP_INIT_PROC(DECODER_RENDER,		decoder_render);
    VDP_INIT_PROC(DECODER_QUERY_CAPABILITIES,	decoder_query_capabilities);

#undef VDP_INIT_PROC

    result = object_heap_init(&driver_data->config_heap, sizeof(struct object_config), CONFIG_ID_OFFSET);
    ASSERT(result == 0);
    if (result != 0)
	return VDPAU_TERMINATE(ctx, ERROR_ALLOCATION_FAILED);

    result = object_heap_init(&driver_data->context_heap, sizeof(struct object_context), CONTEXT_ID_OFFSET);
    ASSERT(result == 0);
    if (result != 0)
	return VDPAU_TERMINATE(ctx, ERROR_ALLOCATION_FAILED);

    result = object_heap_init(&driver_data->surface_heap, sizeof(struct object_surface), SURFACE_ID_OFFSET);
    ASSERT(result == 0);
    if (result != 0)
	return VDPAU_TERMINATE(ctx, ERROR_ALLOCATION_FAILED);

    result = object_heap_init(&driver_data->buffer_heap, sizeof(struct object_buffer), BUFFER_ID_OFFSET);
    ASSERT(result == 0);
    if (result != 0)
	return VDPAU_TERMINATE(ctx, ERROR_ALLOCATION_FAILED);

    result = object_heap_init(&driver_data->output_heap, sizeof(struct object_output), OUTPUT_ID_OFFSET);
    ASSERT(result == 0);
    if (result != 0)
	return VDPAU_TERMINATE(ctx, ERROR_ALLOCATION_FAILED);

    return VA_STATUS_SUCCESS;
}

VAStatus __vaDriverInit_0_29_sds(VADriverContextP ctx)
{
    return vdpau_Initialize(ctx);
}
