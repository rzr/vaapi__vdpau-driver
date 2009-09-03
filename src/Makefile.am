INCLUDES = \
	$(VDPAU_VIDEO_CFLAGS)

LDADD = \
	$(VDPAU_VIDEO_LT_LDFLAGS) -module \
	-no-undefined -module -Wl,--no-undefined

source_h = \
	debug.h		\
	object_heap.h	\
	sysdeps.h	\
	vdpau_video.h

source_c = \
	debug.c		\
	object_heap.c	\
	vdpau_video.c

nvidia_drv_video_la_LTLIBRARIES	= nvidia_drv_video.la
nvidia_drv_video_ladir		= @LIBVA_DRIVERS_PATH@
nvidia_drv_video_la_SOURCES	= $(source_c)
nvidia_drv_video_la_LIBADD	= $(VDPAU_VIDEO_LIBS) -lX11
nvidia_drv_video_la_LDFLAGS	= $(LDADD)

noinst_HEADERS = $(source_h)

# Extra clean files so that maintainer-clean removes *everything*
MAINTAINERCLEANFILES = Makefile.in config.h.in
