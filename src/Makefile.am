## Process this file with automake to produce Makefile.in

bin_PROGRAMS = stellar-core

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

EXTRA_DIST = $(SRC_X_FILES) src.mk
