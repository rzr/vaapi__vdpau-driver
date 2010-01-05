/*
 *  utils.c - Utilities
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

#include "sysdeps.h"
#include "utils.h"
#include <time.h>
#include <errno.h>

#define DEBUG 1
#include "debug.h"


// Parses ENV environment variable for an integer value
int getenv_int(const char *env, int *pval)
{
    const char *env_str = getenv(env);
    if (!env_str)
        return -1;

    char *end = NULL;
    long val = strtoul(env_str, &end, 10);
    if (end == NULL || end[0] != '\0')
        return -1;

    if (pval)
        *pval = val;
    return 0;
}

// Parses ENV environment variable expecting "yes" | "no" values
int getenv_yesno(const char *env, int *pval)
{
    const char *env_str = getenv(env);
    if (!env_str)
        return -1;

    int val;
    if (strcmp(env_str, "1") == 0 || strcmp(env_str, "yes") == 0)
        val = 1;
    else if (strcmp(env_str, "0") == 0 || strcmp(env_str, "no") == 0)
        val = 0;
    else
        return -1;

    if (pval)
        *pval = val;
    return 0;
}

// Get current value of microsecond timer
uint64_t get_ticks_usec(void)
{
#ifdef HAVE_CLOCK_GETTIME
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
#else
    struct timeval t;
    gettimeofday(&t, NULL);
    return (uint64_t)t.tv_sec * 1000000 + t.tv_usec;
#endif
}

#if defined(__linux__)
// Linux select() changes its timeout parameter upon return to contain
// the remaining time. Most other unixen leave it unchanged or undefined.
#define SELECT_SETS_REMAINING
#elif defined(__FreeBSD__) || defined(__sun__) || (defined(__MACH__) && defined(__APPLE__))
#define USE_NANOSLEEP
#elif defined(HAVE_PTHREADS) && defined(sgi)
// SGI pthreads has a bug when using pthreads+signals+nanosleep,
// so instead of using nanosleep, wait on a CV which is never signalled.
#include <pthread.h>
#define USE_COND_TIMEDWAIT
#endif

// Wait for the specified amount of microseconds
void delay_usec(unsigned int usec)
{
    int was_error;

#if defined(USE_NANOSLEEP)
    struct timespec elapsed, tv;
#elif defined(USE_COND_TIMEDWAIT)
    // Use a local mutex and cv, so threads remain independent
    pthread_cond_t delay_cond = PTHREAD_COND_INITIALIZER;
    pthread_mutex_t delay_mutex = PTHREAD_MUTEX_INITIALIZER;
    struct timespec elapsed;
    uint64_t future;
#else
    struct timeval tv;
#ifndef SELECT_SETS_REMAINING
    uint64_t then, now, elapsed;
#endif
#endif

    // Set the timeout interval - Linux only needs to do this once
#if defined(SELECT_SETS_REMAINING)
    tv.tv_sec = 0;
    tv.tv_usec = usec;
#elif defined(USE_NANOSLEEP)
    elapsed.tv_sec = 0;
    elapsed.tv_nsec = usec * 1000;
#elif defined(USE_COND_TIMEDWAIT)
    future = get_ticks_usec() + usec;
    elapsed.tv_sec = future / 1000000;
    elapsed.tv_nsec = (future % 1000000) * 1000;
#else
    then = get_ticks_usec();
#endif

    do {
	errno = 0;
#if defined(USE_NANOSLEEP)
	tv.tv_sec = elapsed.tv_sec;
	tv.tv_nsec = elapsed.tv_nsec;
	was_error = nanosleep(&tv, &elapsed);
#elif defined(USE_COND_TIMEDWAIT)
	was_error = pthread_mutex_lock(&delay_mutex);
	was_error = pthread_cond_timedwait(&delay_cond, &delay_mutex, &elapsed);
	was_error = pthread_mutex_unlock(&delay_mutex);
#else
#ifndef SELECT_SETS_REMAINING
	// Calculate the time interval left (in case of interrupt)
	now = get_ticks_usec();
	elapsed = now - then;
	then = now;
	if (elapsed >= usec)
	    break;
	usec -= elapsed;
	tv.tv_sec = 0;
	tv.tv_usec = usec;
#endif
	was_error = select(0, NULL, NULL, NULL, &tv);
#endif
    } while (was_error && (errno == EINTR));
}

// Reallocates a BUFFER to NUM_ELEMENTS of ELEMENT_SIZE bytes
void *
realloc_buffer(
    void        **buffer_p,
    unsigned int *max_elements_p,
    unsigned int  num_elements,
    unsigned int  element_size
)
{
    if (!buffer_p || !max_elements_p)
        return NULL;

    void *buffer = *buffer_p;
    if (*max_elements_p > num_elements)
        return buffer;

    num_elements += 4;
    if ((buffer = realloc(buffer, num_elements * element_size)) == NULL) {
        free(*buffer_p);
        *buffer_p = NULL;
        return NULL;
    }
    memset((uint8_t *)buffer + *max_elements_p * element_size, 0,
           (num_elements - *max_elements_p) * element_size);

    *buffer_p = buffer;
    *max_elements_p = num_elements;
    return buffer;
}

// Lookup for substring NAME in string EXT using SEP as separators
int find_string(const char *name, const char *ext, const char *sep)
{
    const char *end;
    int name_len, n;

    if (name == NULL || ext == NULL)
        return 0;

    end = ext + strlen(ext);
    name_len = strlen(name);
    while (ext < end) {
        n = strcspn(ext, sep);
        if (n == name_len && strncmp(name, ext, n) == 0)
            return 1;
        ext += (n + 1);
    }
    return 0;
}

// X error trap
static int x11_error_code = 0;
static int (*old_error_handler)(Display *, XErrorEvent *);

static int error_handler(Display *dpy, XErrorEvent *error)
{
    x11_error_code = error->error_code;
    return 0;
}

void x11_trap_errors(void)
{
    x11_error_code    = 0;
    old_error_handler = XSetErrorHandler(error_handler);
}

int x11_untrap_errors(void)
{
    XSetErrorHandler(old_error_handler);
    return x11_error_code;
}

// X window management
static const int x11_event_mask = (KeyPressMask |
                                   KeyReleaseMask |
                                   ButtonPressMask |
                                   ButtonReleaseMask |
                                   PointerMotionMask |
                                   EnterWindowMask |
                                   ExposureMask |
                                   StructureNotifyMask);

Window
x11_create_window(Display *display, unsigned int width, unsigned int height)
{
    Window root_window, window;
    int screen, depth;
    Visual *vis;
    XSetWindowAttributes xswa;
    unsigned long xswa_mask;
    XWindowAttributes wattr;
    unsigned long black_pixel, white_pixel;

    screen      = DefaultScreen(display);
    vis         = DefaultVisual(display, screen);
    root_window = RootWindow(display, screen);
    black_pixel = BlackPixel(display, screen);
    white_pixel = WhitePixel(display, screen);

    XGetWindowAttributes(display, root_window, &wattr);
    depth = wattr.depth;
    if (depth != 15 && depth != 16 && depth != 24 && depth != 32)
        depth = 24;

    xswa_mask             = CWBorderPixel | CWBackPixel;
    xswa.border_pixel     = black_pixel;
    xswa.background_pixel = white_pixel;

    window = XCreateWindow(display, root_window,
                           0, 0, 16, 16, 0, depth,
                           InputOutput, vis, xswa_mask, &xswa);

    if (window)
        XSelectInput(display, window, x11_event_mask);

    return window;
}

void x11_wait_mapped(Display *dpy, Window w)
{
    XEvent e;
    do {
        XMaskEvent(dpy, StructureNotifyMask, &e);
    } while (e.type != MapNotify || e.xmap.event != w);
}

void x11_wait_unmapped(Display *dpy, Window w)
{
    XEvent e;
    do {
        XMaskEvent(dpy, StructureNotifyMask, &e);
    } while (e.type != UnmapNotify || e.xmap.event != w);
}

void x11_wait_exposed(Display *dpy, Window w)
{
    XEvent e;
    do {
        XMaskEvent(dpy, ExposureMask, &e);
    } while (e.type != Expose || e.xexpose.window != w);
}
