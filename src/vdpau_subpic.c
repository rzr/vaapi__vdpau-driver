/*
 *  vdpau_subpic.c - VDPAU backend for VA API (VA subpictures)
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
#include "vdpau_subpic.h"

// vaQuerySubpictureFormats
VAStatus
vdpau_QuerySubpictureFormats(
    VADriverContextP    ctx,
    VAImageFormat      *format_list,
    unsigned int       *flags,
    unsigned int       *num_formats
)
{
    /* TODO */

    if (flags)
        *flags = 0;

    if (num_formats)
        *num_formats = 0;

    return VA_STATUS_SUCCESS;
}

// vaCreateSubpicture
VAStatus
vdpau_CreateSubpicture(
    VADriverContextP    ctx,
    VAImageID           image,
    VASubpictureID     *subpicture
)
{
    /* TODO */
    return VA_STATUS_ERROR_OPERATION_FAILED;
}

// vaDestroySubpicture
VAStatus
vdpau_DestroySubpicture(
    VADriverContextP    ctx,
    VASubpictureID      subpicture
)
{
    /* TODO */
    return VA_STATUS_ERROR_OPERATION_FAILED;
}

// vaSetSubpictureImage
VAStatus
vdpau_SetSubpictureImage(
    VADriverContextP    ctx,
    VASubpictureID      subpicture,
    VAImageID           image
)
{
    /* TODO */
    return VA_STATUS_ERROR_OPERATION_FAILED;
}

// vaSetSubpicturePalette (not a PUBLIC interface)
VAStatus
vdpau_SetSubpicturePalette(
    VADriverContextP    ctx,
    VASubpictureID      subpicture,
    unsigned char      *palette
)
{
    /* TODO */
    return VA_STATUS_ERROR_OPERATION_FAILED;
}

// vaSetSubpictureChromaKey
VAStatus
vdpau_SetSubpictureChromakey(
    VADriverContextP    ctx,
    VASubpictureID      subpicture,
    unsigned int        chromakey_min,
    unsigned int        chromakey_max,
    unsigned int        chromakey_mask
)
{
    /* TODO */
    return VA_STATUS_ERROR_OPERATION_FAILED;
}

// vaSetSubpictureGlobalAlpha
VAStatus
vdpau_SetSubpictureGlobalAlpha(
    VADriverContextP    ctx,
    VASubpictureID      subpicture,
    float               global_alpha
)
{
    /* TODO */
    return VA_STATUS_ERROR_OPERATION_FAILED;
}

// vaAssociateSubpicture
VAStatus
vdpau_AssociateSubpicture(
    VADriverContextP    ctx,
    VASubpictureID      subpicture,
    VASurfaceID        *target_surfaces,
    int                 num_surfaces,
    short               src_x,
    short               src_y,
    short               dest_x,
    short               dest_y,
    unsigned short      width,
    unsigned short      height,
    unsigned int        flags
)
{
    /* TODO */
    return VA_STATUS_ERROR_OPERATION_FAILED;
}

// vaAssociateSubpicture2
VAStatus
vdpau_AssociateSubpicture_full(
    VADriverContextP    ctx,
    VASubpictureID      subpicture,
    VASurfaceID        *target_surfaces,
    int                 num_surfaces,
    short               src_x,
    short               src_y,
    unsigned short      src_width,
    unsigned short      src_height,
    short               dest_x,
    short               dest_y,
    unsigned short      dest_width,
    unsigned short      dest_height,
    unsigned int        flags
)
{
    /* TODO */
    return VA_STATUS_ERROR_OPERATION_FAILED;
}

// vaDeassociateSubpicture
VAStatus
vdpau_DeassociateSubpicture(
    VADriverContextP    ctx,
    VASubpictureID      subpicture,
    VASurfaceID        *target_surfaces,
    int                 num_surfaces
)
{
    /* TODO */
    return VA_STATUS_ERROR_OPERATION_FAILED;
}
