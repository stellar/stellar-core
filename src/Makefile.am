## Process this file with automake to produce Makefile.in

bin_PROGRAMS = stellar-core

include $(top_srcdir)/common.mk
include $(srcdir)/src.mk

noinst_HEADERS = $(SRC_H_FILES)

stellar_core_SOURCES = $(SRC_CXX_FILES)
stellar_core_LDADD = -L$(top_builddir)/lib $(soci_LIBS)		\
	$(libmedida_LIBS) -l3rdparty $(libpq_LIBS) $(xdrpp_LIBS)	\
	$(libsodium_LIBS)

BUILT_SOURCES = $(SRC_X_FILES:.x=.h) StellarCoreVersion.h

SUFFIXES = .x .h
.x.h: $(XDRC)
	$(XDRC) -hh -o $@ $<

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

CLEANFILES = $(BUILT_SOURCES) *~ */*~
MAINTAINERCLEANFILES = $(srcdir)/Makefile.in $(srcdir)/*~ $(srcdir)/*/*~

EXTRA_DIST = $(SRC_X_FILES) src.mk
