DEBIANFILES = \
	changelog.in			\
	compat				\
	control.in			\
	copyright			\
	rules				\
	libva-driver-vdpau.install	\
	$(NULL)

DEBIANGENFILES = \
	changelog			\
	control				\
	$(NULL)

EXTRA_DIST = $(DEBIANFILES)

dist_noinst_DATA = $(DEBIANGENFILES)

# Extra clean files so that maintainer-clean removes *everything*
MAINTAINERCLEANFILES = Makefile.in $(DEBIANGENFILES)

$(DEBIANGENFILES): %: %.in Makefile
	-sed \
	  -e 's|\@PACKAGE_VERSION\@|$(PACKAGE_VERSION)|' \
	  -e 's|\@LIBVA_SDS_PACKAGE_VERSION\@|$(LIBVA_SDS_PACKAGE_VERSION)|' \
	  -e 's|\@DATE\@|'"`LC_ALL=C date +'%a, %d %b %Y %X %z'`"'|' \
	  $< > $@
