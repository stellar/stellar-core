## Process this file with automake to produce Makefile.in

SUBDIRS = . src
AM_EXTRA_RECURSIVE_TARGETS = check-valgrind
ACLOCAL_AMFLAGS = -I m4

# Can't use SUBDIRS for sub-packages as we don't want make install going there
DIST_SUBDIRS = $(SUBDIRS) $(PKGCONFIG_SUBDIRS)
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

lib_lib3rdparty_a_SOURCES = $(SQLITE3_FILES) $(UTIL_FILES)	\
	$(ASIO_CXX_FILES) $(JSON_FILES)
noinst_HEADERS = $(MISC_H_FILES) $(ASIO_H_FILES)

.PHONY: always
always:
	@:

if USE_CLANG_FORMAT
format: always
	cd src && $(MAKE) $(AM_MAKEFLAGS) $@
endif # USE_CLANG_FORMAT

# XXX fix up directory
if USE_AFL_FUZZ
fuzz-testcases: stellar-core
	mkdir -p fuzz-testcases
	for i in `seq 1 10`; do \
	    ./stellar-core --genfuzz fuzz-testcases/fuzz$$i.xdr; \
	done

fuzz: fuzz-testcases stellar-core
	mkdir -p fuzz-findings
	afl-fuzz -m 8000 -i fuzz-testcases -o fuzz-findings \
	    ./stellar-core --fuzz @@

fuzz-clean: always
	rm -Rf fuzz-testcases fuzz-findings
endif # USE_AFL_FUZZ

EXTRA_DIST = stellar-core.supp test/selftest-stellar-core	\
	test/testnet/multitail.conf test/testnet/run-test.sh	\
	lib/lib.mk lib/medida.mk

TESTS=test/selftest-stellar-core

@VALGRIND_CHECK_RULES@
VALGRIND_FLAGS = --num-callers=30 --trace-children=yes \
          --trace-children-skip=hostname,aws,gcutil,gcloud,azure,node,python
VALGRIND_SUPPRESSIONS_FILES = stellar-core.supp

maintainer-clean-local:
	+@echo rm -rf `sed -ne 's!^/!!p' .gitignore` Makefile.in
	rm -rf `sed -ne 's!^/!!p' .gitignore` Makefile.in
if USE_AFL_FUZZ
	+@echo rm -rf fuzz-testcases fuzz-findings
	rm -rf fuzz-testcases fuzz-findings
endif

README: README.md
	$(AM_V_GEN)cp $< $@
