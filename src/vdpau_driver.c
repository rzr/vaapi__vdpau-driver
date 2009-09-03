/*
 *  vdpau_driver.c - VDPAU driver
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
#include "vdpau_driver.h"

#define DEBUG 1
#include "debug.h"


// Return TRUE if underlying VDPAU implementation is NVIDIA
VdpBool
vdpau_is_nvidia(vdpau_driver_data_t *driver_data, int *major, int *minor)
{
    uint32_t nvidia_version = 0;

    if (driver_data->vdp_impl_type == VDP_IMPLEMENTATION_NVIDIA)
        nvidia_version = driver_data->vdp_impl_version;
    if (major)
        *major = nvidia_version >> 16;
    if (minor)
        *minor = nvidia_version & 0xffff;
    return nvidia_version != 0;
}

// Translate VdpStatus to an appropriate VAStatus
VAStatus
vdpau_get_VAStatus(vdpau_driver_data_t *driver_data, VdpStatus vdp_status)
{
    VAStatus va_status;
    const char *vdp_status_string;

    switch (vdp_status) {
    case VDP_STATUS_OK:
        va_status = VA_STATUS_SUCCESS;
        break;
    case VDP_STATUS_NO_IMPLEMENTATION:
        va_status = VA_STATUS_ERROR_UNIMPLEMENTED;
        break;
    case VDP_STATUS_INVALID_CHROMA_TYPE:
        va_status = VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
        break;
    case VDP_STATUS_INVALID_DECODER_PROFILE:
        va_status = VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
        break;
    case VDP_STATUS_RESOURCES:
        va_status = VA_STATUS_ERROR_ALLOCATION_FAILED;
        break;
    default:
        vdp_status_string = vdpau_get_error_string(driver_data, vdp_status);
        D(bug("WARNING: unknown VdpStatus %d: %s\n", vdp_status,
              vdp_status_string ? vdp_status_string : "<unknown error>"));
        va_status = VA_STATUS_ERROR_UNKNOWN;
        break;
    }
    return va_status;
}
