## Process this file with automake to produce Makefile.in

SUBDIRS = $(PKGCONFIG_SUBDIRS)

include $(top_srcdir)/common.mk

$(srcdir)/lib.mk $(srcdir)/libmedida.mk: $(top_srcdir)/make-mks
	cd $(top_srcdir) && ./make-mks

noinst_LIBRARIES = libsoci.a lib3rdparty.a

if LIBMEDIDA_INTERNAL
noinst_LIBRARIES += libmedida.a
include $(srcdir)/medida.mk
libmedida_a_SOURCES = $(INTERNAL_MEDIDA_FILES)
endif # LIBMEDIDA_INTERNAL

include $(srcdir)/lib.mk

libsoci_a_SOURCES = $(SOCI_FILES)
if USE_POSTGRES
libsoci_a_SOURCES += $(SOCI_PG_FILES)
endif # USE_POSTGRES

lib3rdparty_a_SOURCES = $(UTIL_FILES) $(ASIO_CXX_FILES) $(JSON_FILES) $(SQLITE3_FILES)
if USE_SPDLOG
lib3rdparty_a_SOURCES += $(SPDLOG_FILES)
endif # !USE_SPDLOG

if USE_TRACY
lib3rdparty_a_SOURCES += $(TRACY_CXX_FILES)
endif # USE_TRACY
noinst_HEADERS = $(MISC_H_FILES) $(ASIO_H_FILES)

# Suppress any installation of SUBDIRS, which may be distributed separately
install-data:
	@:
install-dvi:
	@:
install-exec: all
	@:
install-html:
	@:
install-info:
	@:
install-pdf:
	@:
install-ps:
	@:
install: all
	@:
installcheck:
	@:
installdirs:
	@:
uninstall:
	@:
