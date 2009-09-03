/*
 *  debug.c - Debugging utilities
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
#include "debug.h"
#include <stdarg.h>

void vdpau_error_message(const char *msg, ...)
{
    va_list args;

    fprintf(stderr, "%s: error: ", PACKAGE_NAME);
    va_start(args, msg);
    vfprintf(stderr, msg, args);
    va_end(args);
}

void vdpau_information_message(const char *msg, ...)
{
    va_list args;

    fprintf(stderr, "%s: ", PACKAGE_NAME);
    va_start(args, msg);
    vfprintf(stderr, msg, args);
    va_end(args);
}

static int g_trace_is_new_line  = 1;
static int g_trace_indent       = 0;

int trace_enabled(void)
{
    static int g_trace_enabled = -1;
    if (g_trace_enabled < 0) {
        if (getenv_yesno("XVBA_VIDEO_TRACE", &g_trace_enabled) < 0)
            g_trace_enabled = 0;
    }
    return g_trace_enabled;
}

static int trace_indent_width(void)
{
    static int g_indent_width = -1;
    if (g_indent_width < 0) {
        if (getenv_int("XVBA_VIDEO_TRACE_INDENT_WIDTH", &g_indent_width) < 0)
            g_indent_width = 4;
    }
    return g_indent_width;
}

void trace_indent(int inc)
{
    g_trace_indent += inc;
}

void trace_print(const char *format, ...)
{
    va_list args;

    if (g_trace_is_new_line) {
        int i, j, n;
        printf("%s: ", PACKAGE_NAME);
        n = trace_indent_width();
        for (i = 0; i < g_trace_indent; i++) {
            for (j = 0; j < n / 4; j++)
                printf("    ");
            for (j = 0; j < n % 4; j++)
                printf(" ");
        }
    }

    va_start(args, format);
    vfprintf(stdout, format, args);
    va_end(args);

    g_trace_is_new_line = (strchr(format, '\n') != NULL);

    if (g_trace_is_new_line)
        fflush(stdout);
}
