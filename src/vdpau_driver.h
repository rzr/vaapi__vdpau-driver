/*
 *  vdpau_driver.h - VDPAU driver
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

#ifndef VDPAU_DRIVER_H
#define VDPAU_DRIVER_H

#include <va/va_backend.h>
#include "vdpau_gate.h"
#include "object_heap.h"

#ifndef VA_INVALID_ID
#define VA_INVALID_ID                   0xffffffff
#endif
#ifndef VA_INVALID_BUFFER
#define VA_INVALID_BUFFER               VA_INVALID_ID
#endif
#ifndef VA_INVALID_SURFACE
#define VA_INVALID_SURFACE              VA_INVALID_ID
#endif
#ifndef VA_STATUS_ERROR_UNIMPLEMENTED
#define VA_STATUS_ERROR_UNIMPLEMENTED   0x00000014
#endif

#define VDPAU_DRIVER_DATA_INIT                           \
        struct vdpau_driver_data *driver_data =          \
            (struct vdpau_driver_data *)ctx->pDriverData

#define VDPAU_OBJECT(id, type) \
    ((object_##type##_p)object_heap_lookup(&driver_data->type##_heap, (id)))

#define VDPAU_CONFIG(id)                VDPAU_OBJECT(id, config)
#define VDPAU_CONTEXT(id)               VDPAU_OBJECT(id, context)
#define VDPAU_SURFACE(id)               VDPAU_OBJECT(id, surface)
#define VDPAU_BUFFER(id)                VDPAU_OBJECT(id, buffer)
#define VDPAU_OUTPUT(id)                VDPAU_OBJECT(id, output)
#define VDPAU_IMAGE(id)                 VDPAU_OBJECT(id, image)
#define VDPAU_GLX_SURFACE(id)           VDPAU_OBJECT(id, glx_surface)

#define VDPAU_CONFIG_ID_OFFSET          0x01000000
#define VDPAU_CONTEXT_ID_OFFSET         0x02000000
#define VDPAU_SURFACE_ID_OFFSET         0x03000000
#define VDPAU_BUFFER_ID_OFFSET          0x04000000
#define VDPAU_OUTPUT_ID_OFFSET          0x05000000
#define VDPAU_IMAGE_ID_OFFSET           0x06000000
#define VDPAU_GLX_SURFACE_ID_OFFSET     0x07000000

#define VDPAU_MAX_PROFILES              12
#define VDPAU_MAX_ENTRYPOINTS           5
#define VDPAU_MAX_CONFIG_ATTRIBUTES     10
#define VDPAU_MAX_IMAGE_FORMATS         10
#define VDPAU_MAX_SUBPICTURE_FORMATS    4
#define VDPAU_MAX_DISPLAY_ATTRIBUTES    4
#define VDPAU_MAX_OUTPUT_SURFACES       2
#define VDPAU_STR_DRIVER_VENDOR         "Splitted-Desktop Systems"
#define VDPAU_STR_DRIVER_NAME           "VDPAU backend for VA API"

typedef enum {
    VDP_IMPLEMENTATION_NVIDIA = 1,
} VdpImplementation;

typedef struct vdpau_driver_data vdpau_driver_data_t;
struct vdpau_driver_data {
    void                       *va_context;
    struct object_heap          config_heap;
    struct object_heap          context_heap;
    struct object_heap          surface_heap;
    struct object_heap          glx_surface_heap;
    struct object_heap          buffer_heap;
    struct object_heap          output_heap;
    struct object_heap          image_heap;
    struct opengl_data         *gl_data;
    VdpDevice                   vdp_device;
    VdpGetProcAddress          *vdp_get_proc_address;
    vdpau_vtable_t              vdp_vtable;
    VdpChromaType               vdp_chroma_format; /* XXX: move elsewhere? */
    VdpImplementation           vdp_impl_type;
    uint32_t                    vdp_impl_version;
    VADisplayAttribute          va_display_attrs[VDPAU_MAX_DISPLAY_ATTRIBUTES];
    unsigned int                va_display_attrs_count;
};

typedef struct object_config   *object_config_p;
typedef struct object_context  *object_context_p;
typedef struct object_surface  *object_surface_p;
typedef struct object_buffer   *object_buffer_p;
typedef struct object_output   *object_output_p;
typedef struct object_image    *object_image_p;

// Return TRUE if underlying VDPAU implementation is NVIDIA
VdpBool
vdpau_is_nvidia(vdpau_driver_data_t *driver_data, int *major, int *minor)
    attribute_hidden;

// Translate VdpStatus to an appropriate VAStatus
VAStatus
vdpau_get_VAStatus(vdpau_driver_data_t *driver_data, VdpStatus vdp_status)
    attribute_hidden;

#endif /* VDPAU_DRIVER_H */

