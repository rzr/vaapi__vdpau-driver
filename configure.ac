# vdpau_video package version number
m4_define([vdpau_video_major_version], [0])
m4_define([vdpau_video_minor_version], [1])
m4_define([vdpau_video_micro_version], [0])
m4_define([vdpau_video_version],
          [vdpau_video_major_version.vdpau_video_minor_version.vdpau_video_micro_version])

# libva package version number
m4_define([libva_api_version], [0.29])

AC_PREREQ([2.57])
AC_INIT([vdpau_video], [vdpau_video_version], [gbeauchesne@splitted-desktop.com], [vdpau-video])
AC_CONFIG_SRCDIR([Makefile.am])
AM_INIT_AUTOMAKE

AM_CONFIG_HEADER([src/config.h])

VDPAU_VIDEO_LT_LDFLAGS="-avoid-version"
AC_SUBST(VDPAU_VIDEO_LT_LDFLAGS)

AC_DISABLE_STATIC
AC_PROG_LIBTOOL
AC_PROG_CC

AC_HEADER_STDC
AC_SYS_LARGEFILE

dnl Check whether VDPAU is available
AC_CACHE_CHECK([for VDPAU],
    ac_cv_have_vdpau, [
    saved_LIBS="$LIBS"
    LIBS="$LIBS -lvdpau"
    AC_TRY_LINK(
    [#include <vdpau/vdpau_x11.h>],
    [vdp_device_create_x11(0,0,0,0)],
    [ac_cv_have_vdpau="yes"; HAVE_VDPAU=1],
    [ac_cv_have_vdpau="no";  HAVE_VDPAU=0; LIBS="$saved_LIBS"])
])
if test "$ac_cv_have_vdpau" = "no"; then
    AC_MSG_ERROR([you need VDPAU to build this package])
fi

PKG_CHECK_MODULES(LIBVA_DEPS, [libva-x11-libva_api_version])
VDPAU_VIDEO_CFLAGS="$LIBVA_DEPS_CFLAGS"
VDPAU_VIDEO_LIBS="$LIBVA_DEPS_LIBS"
AC_SUBST(VDPAU_VIDEO_CFLAGS)
AC_SUBST(VDPAU_VIDEO_LIBS)

AC_OUTPUT([
	Makefile
	src/Makefile
])
