# vdpau_video package version number
m4_define([vdpau_video_major_version], [0])
m4_define([vdpau_video_minor_version], [4])
m4_define([vdpau_video_micro_version], [0])
m4_define([vdpau_video_version],
          [vdpau_video_major_version.vdpau_video_minor_version.vdpau_video_micro_version])

# libva package version number
m4_define([libva_sds_version_0_29], [8])
m4_define([libva_sds_package_version_0_29], [0.29-2+sds11])
m4_define([libva_sds_version_0_30], [1])
m4_define([libva_sds_package_version_0_30], [0.30-1+sds1])
m4_define([libva_sds_version], [libva_sds_version_0_30])
m4_define([libva_sds_package_version], [libva_sds_package_version_0_30])

AC_PREREQ([2.57])
AC_INIT([vdpau_video], [vdpau_video_version], [gbeauchesne@splitted-desktop.com], [vdpau-video])
AC_CONFIG_SRCDIR([Makefile.am])
AM_INIT_AUTOMAKE

AM_CONFIG_HEADER([src/config.h])

LIBVA_SDS_VERSION_0_29=libva_sds_version_0_29
LIBVA_SDS_VERSION_0_30=libva_sds_version_0_30
LIBVA_SDS_VERSION=libva_sds_version
LIBVA_SDS_PACKAGE_VERSION=libva_sds_package_version
AC_SUBST(LIBVA_SDS_PACKAGE_VERSION)

VDPAU_VIDEO_MAJOR_VERSION=vdpau_video_major_version
VDPAU_VIDEO_MINOR_VERSION=vdpau_video_minor_version
VDPAU_VIDEO_MICRO_VERSION=vdpau_video_micro_version
AC_DEFINE([VDPAU_VIDEO_MAJOR_VERSION], [vdpau_video_major_version], [Major version of the driver])
AC_DEFINE([VDPAU_VIDEO_MINOR_VERSION], [vdpau_video_minor_version], [Minor version of the driver])
AC_DEFINE([VDPAU_VIDEO_MICRO_VERSION], [vdpau_video_micro_version], [Micro version of the driver])

VDPAU_VIDEO_LT_LDFLAGS="-avoid-version"
AC_SUBST(VDPAU_VIDEO_LT_LDFLAGS)

AC_DISABLE_STATIC
AC_PROG_LIBTOOL
AC_PROG_CC

AC_C_BIGENDIAN
AC_HEADER_STDC
AC_SYS_LARGEFILE

dnl We want pthreads. Try libpthread first, then libc_r (FreeBSD), then PTL.
HAVE_PTHREADS=yes
AC_CHECK_LIB(pthread, pthread_create, , [
    AC_CHECK_LIB(c_r, pthread_create, , [
        AC_CHECK_LIB(PTL, pthread_create, , [
            HAVE_PTHREADS=no
        ])
    ])
])
if test "x$HAVE_PTHREADS" = "xyes"; then
    AC_DEFINE(HAVE_PTHREADS, 1, [Defined if pthreads are available])
fi

dnl Checks for libraries.
AC_CHECK_LIB(rt, timer_create)

dnl Checks for library functions.
AC_CHECK_FUNCS(clock_gettime)

dnl Check for __attribute__((visibility()))
AC_CACHE_CHECK([whether __attribute__((visibility())) is supported],
  vdpau_cv_visibility_attribute,
  [cat > conftest.c <<EOF
int foo __attribute__ ((visibility ("hidden"))) = 1;
int bar __attribute__ ((visibility ("protected"))) = 1;
EOF
  vdpau_cv_visibility_attribute=no
  if ${CC-cc} -Werror -S conftest.c -o conftest.s >/dev/null 2>&1; then
    if grep '\.hidden.*foo' conftest.s >/dev/null; then
      if grep '\.protected.*bar' conftest.s >/dev/null; then
	vdpau_cv_visibility_attribute=yes
      fi
    fi
  fi
  rm -f conftest.[cs]
])
if test $vdpau_cv_visibility_attribute = yes; then
  AC_DEFINE([HAVE_VISIBILITY_ATTRIBUTE], 1, [Define to use the __attribute__((visibility())) extension])
fi

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

PKG_CHECK_MODULES(LIBVA_DEPS, [libva-x11])
VDPAU_VIDEO_CFLAGS="$LIBVA_DEPS_CFLAGS"
VDPAU_VIDEO_LIBS="$LIBVA_DEPS_LIBS"
AC_SUBST(VDPAU_VIDEO_CFLAGS)
AC_SUBST(VDPAU_VIDEO_LIBS)

dnl Check for SDS extensions to VA API
AC_CACHE_CHECK([for VA API],
  ac_cv_libva_sds_extensions, [
  saved_CFLAGS="$CFLAGS"
  CFLAGS="$CFLAGS $LIBVA_DEPS_CFLAGS"
  AC_TRY_COMPILE([
    #include <va/va_version.h>
    #if VA_MAJOR_VERSION == 0 && VA_MINOR_VERSION == 29
    # if !defined(VA_SDS_VERSION) || (VA_SDS_VERSION < $LIBVA_SDS_VERSION_0_29)
    #  error "VA API version >= 0.29.0-sds$LIBVA_SDS_VERSION_0_29 is required"
    # endif
    #elif VA_MAJOR_VERSION == 0 && VA_MINOR_VERSION == 30
    # if !defined(VA_SDS_VERSION) || (VA_SDS_VERSION < $LIBVA_SDS_VERSION_0_30)
    #  error "VA API version >= 0.30.0-sds$LIBVA_SDS_VERSION_0_30 is required"
    # endif
    #elif !VA_CHECK_VERSION(0,50,0)
    # error "VA API version >= 0.50 is required"
    #endif
  ], [],
  [ac_cv_libva_sds_extensions="yes"],
  [ac_cv_libva_sds_extensions="no"])
  CFLAGS="$saved_CFLAGS"
])
VA_VERSION=`$PKG_CONFIG --modversion libva`
VA_MAJOR_VERSION=`echo "$VA_VERSION" | cut -d'.' -f1`
VA_MINOR_VERSION=`echo "$VA_VERSION" | cut -d'.' -f2`
VA_DRIVER_INIT_FUNC="__vaDriverInit_${VA_MAJOR_VERSION}_${VA_MINOR_VERSION}_sds"
if test "$ac_cv_libva_sds_extensions" = "yes"; then
    AC_DEFINE_UNQUOTED([VA_DRIVER_INIT_FUNC], [$VA_DRIVER_INIT_FUNC], [Define driver entry-point])
else
    AC_MSG_ERROR([Your VA API SDK does not include SDS extensions])
fi

AC_MSG_CHECKING([for VA drivers path])
LIBVA_DRIVERS_PATH=`$PKG_CONFIG libva --variable driverdir`
if test -z "$LIBVA_DRIVERS_PATH"; then
    LIBVA_DRIVERS_PATH="/usr/lib/xorg/modules/drivers"
fi
AC_MSG_RESULT([$LIBVA_DRIVERS_PATH])
AC_SUBST(LIBVA_DRIVERS_PATH)

AC_OUTPUT([
	Makefile
	src/Makefile
])
