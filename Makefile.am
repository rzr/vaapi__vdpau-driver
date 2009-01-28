AUTOMAKE_OPTIONS = foreign
SUBDIRS = src

DEBIANFILES = \
	debian/changelog \
	debian/compat \
	debian/control \
	debian/control.in \
	debian/copyright \
	debian/rules \
	debian/vdpau-video.install

EXTRA_DIST = \
	$(DEBIANFILES)

# Extra clean files so that maintainer-clean removes *everything*
MAINTAINERCLEANFILES = \
	aclocal.m4 compile config.guess config.sub \
	configure depcomp install-sh ltmain.sh     \
	Makefile.in missing debian/control

deb: dist
	-mkdir debian-build
	cd debian-build && \
	tar zxvf ../$(PACKAGE)-$(VERSION).tar.gz && \
	cd $(PACKAGE)-$(VERSION) && dpkg-buildpackage -rfakeroot -uc -us

debian/control: debian/control.in
	-sed \
	  -e 's|\@LIBVA_API_VERSION\@|$(LIBVA_API_VERSION)|' \
	  -e 's|\@LIBVA_SDS_PACKAGE_VERSION\@|$(LIBVA_SDS_PACKAGE_VERSION)|' \
	  $< > $@
