INCLUDES = \
	$(VDPAU_VIDEO_CFLAGS)

LDADD = \
	$(VDPAU_VIDEO_LT_LDFLAGS) -module \
	-no-undefined -module -Wl,--no-undefined

nvidia_drv_video_la_LTLIBRARIES = nvidia_drv_video.la
nvidia_drv_video_ladir = /usr/lib/xorg/modules/drivers
nvidia_drv_video_la_SOURCES = vdpau_video.c object_heap.c debug.c
nvidia_drv_video_la_LIBADD = $(VDPAU_VIDEO_LIBS) -lX11
nvidia_drv_video_la_LDFLAGS = $(LDADD)

noinst_HEADERS = sysdeps.h vdpau_video.h object_heap.h debug.h

# Extra clean files so that maintainer-clean removes *everything*
MAINTAINERCLEANFILES = Makefile.in config.h.in
