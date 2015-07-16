## Process this file with automake to produce Makefile.in

SUBDIRS = . src
AM_EXTRA_RECURSIVE_TARGETS = check-valgrind
ACLOCAL_AMFLAGS = -I m4

include $(top_srcdir)/common.mk

# Can't use SUBDIRS for sub-packages as we don't want make install going there
DIST_SUBDIRS = src $(PKGCONFIG_SUBDIRS)
.PHONY: $(PKGCONFIG_SUBDIRS)
$(PKGCONFIG_SUBDIRS):
	cd $@ && $(MAKE) $(AM_MAKEFLAGS)
all-local: $(PKGCONFIG_SUBDIRS)

noinst_LIBRARIES = lib/libsoci.a lib/lib3rdparty.a

if LIBMEDIDA_INTERNAL
noinst_LIBRARIES += lib/libmedida.a
include $(srcdir)/lib/medida.mk
lib_libmedida_a_SOURCES = $(INTERNAL_MEDIDA_FILES)
endif # LIBMEDIDA_INTERNAL

include $(srcdir)/lib/lib.mk

lib_libsoci_a_SOURCES = $(SOCI_FILES)
if USE_POSTGRES
lib_libsoci_a_SOURCES += $(SOCI_PG_FILES)
endif # USE_POSTGRES

lib_lib3rdparty_a_SOURCES = $(UTIL_FILES) $(ASIO_CXX_FILES) $(JSON_FILES)
if SQLITE3_INTERNAL
lib_lib3rdparty_a_SOURCES += $(SQLITE3_FILES)
endif # SQLITE3_INTERNAL
noinst_HEADERS = $(MISC_H_FILES) $(ASIO_H_FILES)

$(srcdir)/lib/lib.mk $(srcdir)/lib/libmedida.mk: $(top_srcdir)/make-mks
	cd $(top_srcdir) && ./make-mks

.PHONY: always
always:
	@:

if USE_CLANG_FORMAT
format: always
	cd src && $(MAKE) $(AM_MAKEFLAGS) $@
endif # USE_CLANG_FORMAT

if USE_AFL_FUZZ
fuzz-testcases fuzz fuzz-clean: all
	cd src && $(MAKE) $(AM_MAKEFLAGS) $@
endif # USE_AFL_FUZZ

EXTRA_DIST = stellar-core.supp test/selftest-stellar-core	\
	test/testnet/multitail.conf test/testnet/run-test.sh	\
	README.md make-mks

@VALGRIND_CHECK_RULES@
VALGRIND_FLAGS = --num-callers=30 --trace-children=yes \
          --trace-children-skip=hostname,aws,gcutil,gcloud,azure,node,python
VALGRIND_SUPPRESSIONS_FILES = stellar-core.supp

PKGCONFIG_RECURSE = \
	@set -e; target="$@"; target="$${target%%-local}"; \
	for dir in $(PKGCONFIG_SUBDIRS); do \
		(cd "$$dir" && $(MAKE) $(AM_MAKEFLAGS) "$$target"); \
	done

clean-local distclean-local:
	$(PKGCONFIG_RECURSE)

maintainer-clean-local check-local:
	cd $(srcdir) && rm -rf `sed -ne 's!^/!!p' .gitignore` *~ .gitignore~
	$(PKGCONFIG_RECURSE)

CLEANFILES = stellar*.log test-suite.log .gitignore~ *~
