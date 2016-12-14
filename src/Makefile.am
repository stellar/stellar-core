## Process this file with automake to produce Makefile.in

bin_PROGRAMS = stellar-core

include $(top_srcdir)/common.mk
include $(srcdir)/src.mk

noinst_HEADERS = $(SRC_H_FILES)

stellar_core_SOURCES = $(SRC_CXX_FILES)
stellar_core_LDADD = -L$(top_builddir)/lib \
    $(top_builddir)/lib/lib3rdparty.a      \
    $(top_builddir)/lib/libsoci.a          \
    $(libpq_LIBS)

# The stellar_core_LDADD variable is a little tricky, see bug 832
# https://github.com/stellar/stellar-core/issues/832 and relevant
# text in the automake manual on Libraries.
#
# In https://www.gnu.org/software/automake/manual/html_node/Linking.html
#
#   "We recommend that you avoid using -l options in LDADD or prog_LDADD
#    when referring to libraries built by your package. Instead, write the
#    file name of the library explicitly as in the above cpio example. Use
#    -l only to list third-party libraries. If you follow this rule, the
#    default value of prog_DEPENDENCIES will list all your local libraries
#    and omit the other ones."
#
# Essentially, if you list foo.a files in an _LDADD variable, you'll get
# dependency-tracking in-tree, but if you list -lfoo linker arguments
# they'll just get stripped out and you'll have no dependency-tracking (but
# this might be necessary to link against something out-of-tree).
#
# Since our choice between internal and external libraries is made at
# configure-time, we compose our _LDADD variable via automake conditionals
# predicated on those configuration choices.

if SQLITE3_INTERNAL
# if internal, sqlite3 is built into lib3rdparty.a
else
stellar_core_LDADD += $(sqlite3_LIBS)
endif

if LIBMEDIDA_INTERNAL
stellar_core_LDADD += $(top_builddir)/lib/libmedida.a
else
stellar_core_LDADD += $(libmedida_LIBS)
endif

if LIBSODIUM_INTERNAL
stellar_core_LDADD += $(top_builddir)/lib/libsodium/src/libsodium/.libs/libsodium.a
else
stellar_core_LDADD += $(libsodium_LIBS)
endif

if XDRPP_INTERNAL
stellar_core_LDADD += $(top_builddir)/lib/xdrpp/xdrpp/libxdrpp.a
else
stellar_core_LDADD += $(xdrpp_LIBS)
endif

BUILT_SOURCES = $(SRC_X_FILES:.x=.h) StellarCoreVersion.h

SUFFIXES = .x .h
.x.h: $(XDRC)
	$(XDRC) -hh -o $@ $<

$(srcdir)/src.mk: $(top_srcdir)/make-mks
	cd $(top_srcdir) && ./make-mks

.PHONY: always
always:
	@:

# Always rebuild because .git/HEAD is a symbolic ref one can't depend on
StellarCoreVersion.h: always
	@vers=$$(cd "$(srcdir)" \
		&& git describe --always --dirty --tags 2>/dev/null \
		|| echo "$(PACKAGE) $(VERSION)"); \
	    echo "#define STELLAR_CORE_VERSION \"$$vers\"" > $@~
	@if cmp -s $@~ $@; then rm -f $@~; else \
	    mv -f $@~ $@ && printf "echo '%s' > $@\n" "$$(cat $@)"; fi

if USE_POSTGRES
TESTS=test/selftest-pg
else # !USE_POSTGRES
TESTS=test/selftest-nopg
endif # !USE_POSTGRES

if USE_CLANG_FORMAT
format: always
	cd $(srcdir) && $(CLANG_FORMAT) -i $(SRC_CXX_FILES) $(SRC_H_FILES)
endif # USE_CLANG_FORMAT

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

distclean-local: fuzz-clean
endif # USE_AFL_FUZZ

CLEANFILES = $(BUILT_SOURCES) *~ */*~ stellar*.log
MAINTAINERCLEANFILES = $(srcdir)/Makefile.in $(srcdir)/*~ $(srcdir)/*/*~

EXTRA_DIST = $(SRC_X_FILES) test/selftest-nopg test/selftest-pg
