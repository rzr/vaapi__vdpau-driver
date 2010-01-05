/*
 *  utils.h - Utilities
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

#ifndef UTILS_H
#define UTILS_H

#include <X11/Xlib.h>

int getenv_int(const char *env, int *pval)
    attribute_hidden;

int getenv_yesno(const char *env, int *pval)
    attribute_hidden;

uint64_t get_ticks_usec(void)
    attribute_hidden;

void delay_usec(unsigned int usec)
    attribute_hidden;

void *
realloc_buffer(
    void        **buffer_p,
    unsigned int *max_elements_p,
    unsigned int  num_elements,
    unsigned int  element_size
) attribute_hidden;

int find_string(const char *name, const char *ext, const char *sep)
    attribute_hidden;

void x11_trap_errors(void)
    attribute_hidden;

int x11_untrap_errors(void)
    attribute_hidden;

Window
x11_create_window(Display *display, unsigned int width, unsigned int height)
    attribute_hidden;

void x11_wait_mapped(Display *dpy, Window w)
    attribute_hidden;

void x11_wait_unmapped(Display *dpy, Window w)
    attribute_hidden;

void x11_wait_exposed(Display *dpy, Window w)
    attribute_hidden;

#endif /* UTILS_H */
