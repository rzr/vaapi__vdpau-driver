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

#if USE_GLX
#include <GL/gl.h>
#include <GL/glx.h>
#include <GL/glext.h>
#endif

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

typedef enum {
    OPENGL_STATUS_NONE  = 0,
    OPENGL_STATUS_OK    = 1,
    OPENGL_STATUS_ERROR = -1
} OpenGLStatus;

typedef struct opengl_data opengl_data_t;
struct opengl_data {
    OpenGLStatus                        gl_status;
#if USE_GLX
    PFNGLXBINDTEXIMAGEEXTPROC           glx_bind_tex_image;
    PFNGLXRELEASETEXIMAGEEXTPROC        glx_release_tex_image;
    PFNGLGENFRAMEBUFFERSEXTPROC         gl_gen_framebuffers;
    PFNGLDELETEFRAMEBUFFERSEXTPROC      gl_delete_framebuffers;
    PFNGLBINDFRAMEBUFFEREXTPROC         gl_bind_framebuffer;
    PFNGLGENRENDERBUFFERSEXTPROC        gl_gen_renderbuffers;
    PFNGLDELETERENDERBUFFERSEXTPROC     gl_delete_renderbuffers;
    PFNGLBINDRENDERBUFFEREXTPROC        gl_bind_renderbuffer;
    PFNGLRENDERBUFFERSTORAGEEXTPROC     gl_renderbuffer_storage;
    PFNGLFRAMEBUFFERRENDERBUFFEREXTPROC gl_framebuffer_renderbuffer;
    PFNGLFRAMEBUFFERTEXTURE2DEXTPROC    gl_framebuffer_texture_2d;
    PFNGLCHECKFRAMEBUFFERSTATUSEXTPROC  gl_check_framebuffer_status;
#endif
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
};

typedef struct object_glx_surface object_glx_surface_t;
struct object_glx_surface {
    struct object_base           base;
#if USE_GLX
    GLenum                       target;
    GLuint                       texture;
    VASurfaceID                  va_surface;
    unsigned int                 width;
    unsigned int                 height;
    int                          is_bound;
    Pixmap                       pixmap;
    GLXPixmap                    glx_pixmap;
    GLuint                       fbo;
    GLuint                       fbo_buffer;
    GLuint                       fbo_texture;
#endif
};

typedef struct object_buffer object_buffer_t;
struct object_buffer {
    struct object_base           base;
    VABufferType                 type;
    void                        *buffer_data;
    uint32_t                     buffer_size;
    int                          max_num_elements;
    int                          num_elements;
};

typedef struct object_output object_output_t;
struct object_output {
    struct object_base           base;
    Drawable                     drawable;
    uint32_t                     width;
    uint32_t                     height;
    VdpPresentationQueue         vdp_flip_queue;
    VdpPresentationQueueTarget   vdp_flip_target;
    uint32_t                     output_surface_width;
    uint32_t                     output_surface_height;
    VdpOutputSurface             vdp_output_surfaces[VDPAU_MAX_OUTPUT_SURFACES];
    int                          current_output_surface;
};

typedef struct object_image object_image_t;
struct object_image {
    struct object_base           base;
    VAImage                     *image;
    VdpOutputSurface             vdp_rgba_surface;
};

typedef object_glx_surface_t    *object_glx_surface_p;

#endif /* VDPAU_VIDEO_H */
