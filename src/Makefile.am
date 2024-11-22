## Process this file with automake to produce Makefile.in

AUTOMAKE_OPTIONS = serial-tests
bin_PROGRAMS = stellar-core

include $(top_srcdir)/common.mk
include $(srcdir)/src.mk

noinst_HEADERS = $(SRC_H_FILES)

# We sometimes specify a CXXFLAGS setting like -stdlib=libc++ that causes the
# stdlib to change. When this makes it way down to Rust code building C++ code
# for one of its libraries (specifically tracy-client-sys) this needs to be
# accompanied by an _explicit_ instruction to invoke the linker with -lc++. This
# is done by setting the CXXSTDLIB flag, which Rust's C++-building machinery is
# sensitive to. Rust passes-on, but does not look inside, CXXFLAGS itself to
# realize that it needs this setting.
CXXSTDLIB := $(if $(findstring -stdlib=libc++,$(CXXFLAGS)),c++,$(if $(findstring -stdlib=libstdc++,$(CXXFLAGS)),stdc++,))

if USE_TRACY
# NB: this unfortunately long list has to be provided here and kept in sync with
# the list of features for tracy-client in src/rust/Cargo.toml.  This is because
# the soroban sub-crates are built independently and know nothing about
# src/rust/Cargo.toml
#
# NB also: this list does _not_ and should not include the manual-lifetime
# feature. Versions of the rust tracy-client crate before 0.17 (eg. version
# 0.16.4 which we shipped in soroban 21.x) always used the C++ tracy code in
# manual-lifetime mode, and thus required careful coordination with
# stellar-core's C++ code to start and stop tracy correctly. However this became
# intractable when we started building multiple soroban libraries as .rlibs with
# separate copies of tracy-client simultaneously, as they would each have their
# own rust-metadata-hash-qualified copy of a rust global variable tracking the
# initialization state of tracy (and of course tracy's C++ code itself cannot be
# started multiple times idempotently in manual-lifetime mode, that would be too
# easy). So we have now disabled tracy-client (0.16.4) in soroban 21.x and only
# enabled it in soroban 22.x (and later), which has tracy-client 0.17, which
# finally supports _turning off_ manual-lifetime mode. Once off, tracy's C++
# code initializes itself on its own, and everything works fine. There is only
# one copy of tracy in the final linked executable and it has a single atomic
# pointer controlling its initialization state.
CARGO_FEATURE_TRACY = --features tracy
CARGO_FEATURE_TRACY += --features tracy-client/enable
CARGO_FEATURE_TRACY += --features tracy-client/ondemand
CARGO_FEATURE_TRACY += --features tracy-client/delayed-init
CARGO_FEATURE_TRACY += --features tracy-client/system-tracing
CARGO_FEATURE_TRACY += --features tracy-client/sampling
CARGO_FEATURE_TRACY += --features tracy-client/code-transfer
CARGO_FEATURE_TRACY += --features tracy-client/timer-fallback
CARGO_FEATURE_TRACY += --features tracy-client/only-localhost
CARGO_FEATURE_TRACY += --features tracy-client/only-ipv4
else
CARGO_FEATURE_TRACY =
endif

if BUILD_TESTS
stellar_core_SOURCES = main/StellarCoreVersion.cpp main/XDRFilesSha256.cpp $(SRC_CXX_FILES) $(SRC_TEST_CXX_FILES)
CARGO_FEATURE_TESTUTILS = --features testutils
else # !BUILD_TESTS
stellar_core_SOURCES = main/StellarCoreVersion.cpp main/XDRFilesSha256.cpp $(SRC_CXX_FILES)
CARGO_FEATURE_TESTUTILS =
endif # !BUILD_TESTS


if ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
CARGO_FEATURE_NEXT = --features next
main/XDRFilesSha256.cpp: $(SRC_X_FILES) Makefile $(top_srcdir)/hash-xdrs.sh
	$(top_srcdir)/hash-xdrs.sh protocol-next >$@
else
CARGO_FEATURE_NEXT =
main/XDRFilesSha256.cpp: $(SRC_X_FILES) Makefile $(top_srcdir)/hash-xdrs.sh
	$(top_srcdir)/hash-xdrs.sh protocol-curr >$@
endif

stellar_core_LDADD = $(soci_LIBS) $(libmedida_LIBS)		\
	$(top_builddir)/lib/lib3rdparty.a $(sqlite3_LIBS)	\
	$(libpq_LIBS) $(xdrpp_LIBS) $(libsodium_LIBS) $(libunwind_LIBS)

TESTDATA_DIR = testdata
TEST_FILES = $(TESTDATA_DIR)/stellar-core_example.cfg $(TESTDATA_DIR)/stellar-core_standalone.cfg \
             $(TESTDATA_DIR)/stellar-core_testnet.cfg $(TESTDATA_DIR)/stellar-core_testnet_legacy.cfg \
             $(TESTDATA_DIR)/stellar-history.testnet.6714239.json $(TESTDATA_DIR)/stellar-history.livenet.15686975.json \
             $(TESTDATA_DIR)/stellar-core_testnet_validator.cfg $(TESTDATA_DIR)/stellar-core_example_validators.cfg \
             $(TESTDATA_DIR)/stellar-history.testnet.6714239.networkPassphrase.json

BUILT_SOURCES = $(SRC_X_FILES:.x=.h) main/StellarCoreVersion.cpp main/XDRFilesSha256.cpp $(TEST_FILES)

$(SRC_X_FILES:.x=.h): $(XDRC)
SUFFIXES = .x .h .rs
.x.h:
	$(XDRC) -hh -pedantic -o $@ $<

BISON=bison
FLEX=flex

util/xdrquery/XDRQueryScanner.cpp: util/xdrquery/XDRQueryScanner.ll
	$(FLEX) --outfile=$@ $<

util/xdrquery/XDRQueryParser.cpp: util/xdrquery/XDRQueryParser.yy
	$(BISON) --defines=util/xdrquery/XDRQueryParser.h  --output=$@ $<

util/xdrquery/XDRQueryParser.h: util/xdrquery/XDRQueryParser.cpp
	touch $@

BUILT_SOURCES += util/xdrquery/XDRQueryScanner.cpp util/xdrquery/XDRQueryParser.h util/xdrquery/XDRQueryParser.cpp
stellar_core_SOURCES += util/xdrquery/XDRQueryScanner.cpp util/xdrquery/XDRQueryParser.h util/xdrquery/XDRQueryParser.cpp

# Old automakes have buggy dependency tracking for conditional generated
# sources. We work around this here by making rust/RustBridge.{cpp,h} generated
# in all cases, and just empty in the non-rust case. Also because of the way old
# automake relies on horrible path-munging code in config.status to extract the
# names of depfiles from the Makefile itself, we can't use any variables in the
# SOURCES addition we're doing here, have to list unadorned paths.

BUILT_SOURCES += rust/RustBridge.h rust/RustBridge.cpp
stellar_core_SOURCES += rust/RustBridge.h rust/RustBridge.cpp

RUST_TOOLCHAIN_FILE=$(top_srcdir)/rust-toolchain.toml
RUST_TOOLCHAIN_CHANNEL=$(shell sed -n 's/channel *= *"\([^"]*\)"/\1/p' $(RUST_TOOLCHAIN_FILE))
CARGO=cargo +$(RUST_TOOLCHAIN_CHANNEL)

RUST_BUILD_DIR=$(top_builddir)/src/rust
RUST_BIN_DIR=$(RUST_BUILD_DIR)/bin
RUST_TARGET_DIR=$(top_builddir)/target
RUST_CXXBRIDGE=$(RUST_BIN_DIR)/cxxbridge
RUST_PROFILE=release
RUST_DEP_TREE_STAMP=$(RUST_BUILD_DIR)/src/dep-trees/equal-trees.stamp
SOROBAN_LIBS_STAMP=$(RUST_BUILD_DIR)/soroban/soroban-libs.stamp
RUST_HOST_DEPFILES=rust/Cargo.toml $(top_srcdir)/Cargo.toml $(top_srcdir)/Cargo.lock $(RUST_DEP_TREE_STAMP)
LIBRUST_STELLAR_CORE=$(RUST_TARGET_DIR)/$(RUST_PROFILE)/librust_stellar_core.a
stellar_core_LDADD += $(LIBRUST_STELLAR_CORE) -ldl

SOROBAN_BUILD_DIR=$(abspath $(RUST_BUILD_DIR))/soroban
ALL_SOROBAN_PROTOCOLS=$(notdir $(wildcard rust/soroban/p*))
SOROBAN_MAX_PROTOCOL=$(lastword $(sort $(ALL_SOROBAN_PROTOCOLS)))

define soroban_lib_dir
$(shell printf '$(SOROBAN_BUILD_DIR)/%s/target/$(RUST_PROFILE)' $(1))
endef

define soroban_rlib
$(shell printf -- '%s/libsoroban_env_host.rlib' $(call soroban_lib_dir,$(1)))
endef

define soroban_extern_flag
$(shell printf -- '--extern soroban_env_host_%s=%s' $(1) $(call soroban_rlib,$(1)))
endef

define soroban_depend_flag
$(shell printf -- '-L dependency=%s/deps' $(call soroban_lib_dir,$(1)))
endef

ALL_SOROBAN_LIBS=$(foreach proto,$(ALL_SOROBAN_PROTOCOLS),$(call soroban_rlib,$(proto)))
ALL_SOROBAN_EXTERN_ARGS=$(foreach proto,$(ALL_SOROBAN_PROTOCOLS),$(call soroban_extern_flag,$(proto)))
ALL_SOROBAN_DEPEND_ARGS=$(foreach proto,$(ALL_SOROBAN_PROTOCOLS),$(call soroban_depend_flag,$(proto)))

$(RUST_CXXBRIDGE): Makefile $(RUST_TOOLCHAIN_FILE)
	mkdir -p $(RUST_BIN_DIR)
	CARGO_HTTP_MULTIPLEXING=false $(CARGO) install --force --locked --root $(RUST_BUILD_DIR) cxxbridge-cmd --version 1.0.68

rust/RustBridge.h: rust/src/lib.rs $(SRC_RUST_FILES) Makefile $(RUST_CXXBRIDGE)
	$(RUST_CXXBRIDGE) $< --cfg test=false --header --output $@.tmp
	if cmp -s $@.tmp $@; then rm -v $@.tmp; else mv -v $@.tmp $@; fi

rust/RustBridge.cpp: rust/src/lib.rs $(SRC_RUST_FILES) Makefile $(RUST_CXXBRIDGE)
	$(RUST_CXXBRIDGE) $< --cfg test=false --output $@.tmp
	if cmp -s $@.tmp $@; then rm -v $@.tmp; else mv -v $@.tmp $@; fi

# This is just a convenience target for rebuilding the explicit depfiles we
# check in to this repo as a secondary check on the lockfiles in the soroban
# submodules. You should still carefully inspect them if they change!

$(RUST_DEP_TREE_STAMP): $(wildcard rust/soroban/*/Cargo.*) Makefile $(RUST_TOOLCHAIN_FILE)
	rm -f $@
	for proto in $(ALL_SOROBAN_PROTOCOLS); \
	do \
		$(CARGO) tree --manifest-path rust/soroban/$${proto}/Cargo.toml --locked --package soroban-env-host --edges no-dev --target all \
		| sed -e "s@$(abspath $(top_srcdir))/@@g" > $(RUST_BUILD_DIR)/src/dep-trees/$${proto}-actual.txt ; \
		if ! diff -u rust/src/dep-trees/$${proto}-expect.txt $(RUST_BUILD_DIR)/src/dep-trees/$${proto}-actual.txt; \
		then \
			echo "dep trees differ, please update $${proto}-expect.txt or roll back submodule"; \
			exit 1; \
		fi; \
	done
	touch $@

# This next build command looks a little weird but it's necessary. We have to
# provide an auxiliary metadata string (using RUSTFLAGS=-Cmetadata=$*)
# essentially manually marking-as-different the separate dependency trees
# associated with each copy of soroban. If we don't do this, then for various
# indirect dependencies of soroban -- dependencies that more-or-less _don't_
# differ between soroban versions -- we will wind up with two copies of the
# dependency's rlib with the same `StableCrateId` (a hash that rust uses to
# identify crates-and-versions) but with non-bit-identical content (due to
# various minor non-reproducible-build idiosyncrasies). And this "multiple
# StableCrateIds with bit-different content" situation in turn is considered a
# fatal error when trying to load that `StableCrateId` into the resulting
# binary, as we will be doing when we link multiple sorobans together. Rustc
# just crashes with a "StableCrateId collision" error.
#
# So to avoid that we make sure that _every_ time we compile a soroban, its
# indirect deps are always marked as _more different_ than they actually are.
# I.e. if the same version of the same dependency appears in soroban p21 and
# p22, they _would_ normally get called the same StableCrateId, but we pass
# RUSTFLAGS=-Cmetadata=p21 for one of them and RUSTFLAGS=-Cmetadata=p22 for the
# other, and that causes their "identities" to be considered distinct.
#
# Interestingly this does not appear to cause the final binary to get any
# bigger, so I guess LTO somehow identifies the duplicate symbols and merges
# them back together. Or something. Anyways it works and is the only portable
# and robust solution we've found to preventing the StableCrateId collision.
#
# We also have to be somewhat selective about the versions we pass the `next`
# feature to (only the most recent soroban) and the versions we pass the `tracy`
# features to (only those post-p22 versions that support the tracy delayed-init
# feature).
#
# We also have to sequentialize the builds of the sorobans after the
# dep-tree-stamp files and before the stellar-core build, because rustup gets
# invoked and it is not concurrency-safe.
#
# We also have to set pass all the CC, CXX, CFLAGS and CXXFLAGS we were called
# with explicitly, because make does not implicitly export them to its rules,
# but cargo needs them to build C++ files in build.rs files.
#
# We also have to pass CXXSTDLIB to those build.rs files, because they are
# sensitive to CXXFLAGS but also don't inspect them to see if they're setting
# -stdlib=libc++ or -stdlib=libstdc++
$(SOROBAN_LIBS_STAMP): $(wildcard rust/soroban/p*/Cargo.lock) Makefile $(RUST_DEP_TREE_STAMP) $(SRC_RUST_FILES) $(RUST_TOOLCHAIN_FILE)
	rm -f $@
	for proto in $(ALL_SOROBAN_PROTOCOLS) ; \
	do \
		FEATURE_FLAGS="" ; \
		case "$$proto" in \
			p21) \
				FEATURE_FLAGS="" \
			;; \
			$(SOROBAN_MAX_PROTOCOL)) \
				FEATURE_FLAGS="$(CARGO_FEATURE_TRACY) $(CARGO_FEATURE_NEXT)" \
			;; \
			*) \
				FEATURE_FLAGS="$(CARGO_FEATURE_TRACY)" \
			;; \
		esac ; \
		cd $(abspath $(RUST_BUILD_DIR))/soroban/$$proto && \
		CC="$(CC)" CXX="$(CXX)" LD="$(LD)" CFLAGS="$(CFLAGS)" CXXFLAGS="$(CXXFLAGS)" CXXSTDLIB="$(CXXSTDLIB)" LDFLAGS="$(LDFLAGS)" \
		RUSTFLAGS="-Cmetadata=$$proto $(RUSTFLAGS_ASAN)" \
		CARGO_NET_GIT_FETCH_WITH_CLI=true \
		$(CARGO) build \
			--package soroban-env-host \
			--$(RUST_PROFILE) \
			--locked \
			$$FEATURE_FLAGS \
		|| exit 1; \
	done
	touch $@

# This one is also a bit challenging, though it's _less_ weird. What we're doing
# here is supplying multiple separate .rlibs -- one for each soroban linked into
# the binary -- to the crate build for the `stellar-core` crate. This is
# necessary because we want to use the _exact_ versions locked into each
# soroban, and we want to avoid any possibility of cargo/rustc trying to merge
# dependencies between the sorobans (which it frequently does in ways we
# otherwise can't stop, eg. merging minor versions of indirect dependencies).
#
# We want to treat them as strictly separate and strictly fixed dependency
# graphs, so we compile them separately into .rlibs (see above) and then pass
# them in as separate `--extern` arguments to a slightly-more-manual `cargo
# rustc` invocation, along with `-L dependency=...` flags to tell cargo where to
# find indirect deps of those .rlibs.
$(LIBRUST_STELLAR_CORE): $(RUST_HOST_DEPFILES) $(SRC_RUST_FILES) Makefile $(SOROBAN_LIBS_STAMP) $(RUST_TOOLCHAIN_FILE)
	rm -rf $(abspath $(RUST_TARGET_DIR))
	CC="$(CC)" CXX="$(CXX)" LD="$(LD)" CFLAGS="$(CFLAGS)" CXXFLAGS="$(CXXFLAGS)" CXXSTDLIB="$(CXXSTDLIB)" LDFLAGS="$(LDFLAGS)" \
	RUSTFLAGS="$(RUSTFLAGS_ASAN)" \
	CARGO_NET_GIT_FETCH_WITH_CLI=true \
	$(CARGO) rustc \
		--package stellar-core \
		--$(RUST_PROFILE) \
		--locked \
		--target-dir $(abspath $(RUST_TARGET_DIR)) \
		$(CARGO_FEATURE_TRACY) $(CARGO_FEATURE_NEXT) $(CARGO_FEATURE_TESTUTILS) \
		-- \
		$(ALL_SOROBAN_EXTERN_ARGS) \
		$(ALL_SOROBAN_DEPEND_ARGS)
	ranlib $@

$(srcdir)/src.mk: $(top_srcdir)/make-mks
	cd $(top_srcdir) && ./make-mks

$(TESTDATA_DIR)/%.cfg : $(top_srcdir)/docs/%.cfg
	mkdir -p $(@D) && cp $< $@

$(TESTDATA_DIR)/%.json : $(top_srcdir)/src/history/serialize-tests/%.json
	mkdir -p $(@D) && cp $< $@

.PHONY: always
always:
	@:

# Always rebuild because .git/HEAD is a symbolic ref one can't depend on
main/StellarCoreVersion.cpp: always
	@vers=$$(cd "$(srcdir)" \
		&& git describe --always --dirty --tags 2>/dev/null \
		|| echo "$(PACKAGE) $(VERSION)"); \
		sed -e "s@%%VERSION%%@$$vers@" \
			< "$(srcdir)/main/StellarCoreVersion.cpp.in" > $@~
	@if cmp -s $@~ $@; then rm -f $@~; else \
	    mv -f $@~ $@ && printf "echo '%s' > $@\n" "$$(cat $@)"; fi

if USE_POSTGRES
TESTS=test/selftest-pg
else # !USE_POSTGRES
TESTS=test/selftest-nopg
endif # !USE_POSTGRES
TESTS += test/check-nondet

format: always
if USE_CLANG_FORMAT
	cd $(srcdir) && $(CLANG_FORMAT) -style=file -i $(SRC_CXX_FILES) $(SRC_H_FILES) $(SRC_TEST_CXX_FILES) $(SRC_TEST_H_FILES)
endif # USE_CLANG_FORMAT
	cd $(srcdir) && $(CARGO) fmt --all

if USE_AFL_FUZZ
FUZZER_MODE ?= overlay

fuzz-testcases: stellar-core
	mkdir -p fuzz-testcases
	for i in `seq 1 10000`; do \
	    ./stellar-core gen-fuzz --mode=${FUZZER_MODE} fuzz-testcases/fuzz$$i.xdr ; \
	done
	mkdir -p min-testcases
	afl-cmin -i fuzz-testcases -o min-testcases -m 500 -t 250 ./stellar-core fuzz --ll ERROR --mode=${FUZZER_MODE} @@
	rm -Rf fuzz-testcases

# when running in parallel,
# run the same command than below replacing `-M main` with `-S worker_N`
# and `--process-id 0` with `--process-id N`

fuzz: fuzz-testcases stellar-core
	mkdir -p fuzz-findings
	afl-fuzz -m 500 -M main -t 250 -i min-testcases -o fuzz-findings \
	    ./stellar-core fuzz --ll ERROR --process-id 0 --mode=${FUZZER_MODE} @@

fuzz-clean: always
	rm -Rf fuzz-testcases fuzz-findings

distclean-local: fuzz-clean
endif # USE_AFL_FUZZ

clean-local:
	rm -rf $(top_builddir)/target $(top_builddir)/src/rust/soroban/*/target $(RUST_DEP_TREE_STAMP) $(SOROBAN_LIBS_STAMP)

CLEANFILES = $(BUILT_SOURCES) *~ */*~ stellar*.log
MAINTAINERCLEANFILES = $(srcdir)/Makefile.in $(srcdir)/*~ $(srcdir)/*/*~

EXTRA_DIST = $(SRC_X_FILES) test/selftest-nopg test/selftest-pg
