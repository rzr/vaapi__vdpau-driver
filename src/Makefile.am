INCLUDES = \
	$(VDPAU_VIDEO_CFLAGS)

LDADD = \
	$(VDPAU_VIDEO_LT_LDFLAGS) -module \
	-no-undefined -module -Wl,--no-undefined

source_glx_h = vdpau_video_glx.h
source_glx_c = vdpau_video_glx.c

source_x11_h = vdpau_video_x11.h
source_x11_c = vdpau_video_x11.c

source_h = \
	debug.h		\
	object_heap.h	\
	sysdeps.h	\
	utils.h		\
	vdpau_buffer.h	\
	vdpau_decode.h	\
	vdpau_driver.h	\
	vdpau_dump.h	\
	vdpau_gate.h	\
	vdpau_image.h	\
	vdpau_subpic.h	\
	vdpau_video.h	\
	$(source_glx_h)	\
	$(source_x11_h)

source_c = \
	debug.c		\
	object_heap.c	\
	utils.c		\
	vdpau_buffer.c	\
	vdpau_decode.c	\
	vdpau_driver.c	\
	vdpau_dump.c	\
	vdpau_gate.c	\
	vdpau_image.c	\
	vdpau_subpic.c	\
	vdpau_video.c	\
	$(source_glx_c)	\
	$(source_x11_c)

nvidia_drv_video_la_LTLIBRARIES	= nvidia_drv_video.la
nvidia_drv_video_ladir		= @LIBVA_DRIVERS_PATH@
nvidia_drv_video_la_SOURCES	= $(source_c)
nvidia_drv_video_la_LIBADD	= $(VDPAU_VIDEO_LIBS) -lX11
nvidia_drv_video_la_LDFLAGS	= $(LDADD)

noinst_HEADERS = $(source_h)

# Extra clean files so that maintainer-clean removes *everything*
MAINTAINERCLEANFILES = Makefile.in config.h.in
