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

# Generated sources that are built separately (not in SRC_CXX_FILES)
# These are reused by both stellar-core and fuzz targets.
GENERATED_VERSION_SOURCES = main/StellarCoreVersion.cpp main/XDRFilesSha256.cpp
GENERATED_XDRQUERY_SOURCES = util/xdrquery/XDRQueryScanner.cpp util/xdrquery/XDRQueryParser.cpp
GENERATED_RUST_SOURCES = rust/RustBridge.cpp

# main/main.cpp is excluded from SRC_CXX_FILES (via make-mks) so fuzz targets
# don't get the stellar-core main(). We add it back here for stellar-core only.
MAIN_FILES = $(GENERATED_VERSION_SOURCES) main/main.cpp

if BUILD_TESTS
stellar_core_SOURCES = $(MAIN_FILES) $(SRC_CXX_FILES) $(SRC_TEST_CXX_FILES)
CARGO_FEATURE_TESTUTILS = --features testutils
# Pass testutils to soroban max protocol so fuzz targets can be smoke tested
SOROBAN_MAX_PROTOCOL_TESTUTILS_FLAGS = --features testutils
else # !BUILD_TESTS
stellar_core_SOURCES = $(MAIN_FILES) $(SRC_CXX_FILES)
CARGO_FEATURE_TESTUTILS =
SOROBAN_MAX_PROTOCOL_TESTUTILS_FLAGS =
endif # !BUILD_TESTS

# ENABLE_FUZZ controls fuzz *instrumentation* and fuzz binary building.
# The fuzz *targets* themselves (the logic to test) are available under
# testutils, so they can be smoke-tested in normal BUILD_TESTS builds.
# ENABLE_FUZZ adds:
# - FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION define (bypasses signature checks, etc)
# - Building fuzz_* binaries
# - Linking against LIB_FUZZING_ENGINE
if ENABLE_FUZZ
# Keep the fuzz feature for any instrumentation-specific code, though currently
# it just enables testutils in the soroban crate.
CARGO_FEATURE_FUZZ = --features fuzz
else
CARGO_FEATURE_FUZZ =
endif


if ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
CARGO_FEATURE_NEXT = --features next
main/XDRFilesSha256.cpp: $(SRC_X_FILES) Makefile $(top_srcdir)/hash-xdrs.sh
	$(top_srcdir)/hash-xdrs.sh $(top_srcdir)/src/protocol-next >$@
else
CARGO_FEATURE_NEXT =
main/XDRFilesSha256.cpp: $(SRC_X_FILES) Makefile $(top_srcdir)/hash-xdrs.sh
	$(top_srcdir)/hash-xdrs.sh $(top_srcdir)/src/protocol-curr >$@
endif

# tcmalloc must be linked early to properly override malloc/free
stellar_core_LDADD = $(libtcmalloc_LIBS) $(soci_LIBS) $(libmedida_LIBS)		\
	$(top_builddir)/lib/lib3rdparty.a $(sqlite3_LIBS)	\
	$(libpq_LIBS) $(xdrpp_LIBS) $(libsodium_LIBS)

TESTDATA_DIR = testdata
TEST_FILES = $(TESTDATA_DIR)/stellar-core_example.cfg $(TESTDATA_DIR)/stellar-core_standalone.cfg \
             $(TESTDATA_DIR)/stellar-core_testnet.cfg $(TESTDATA_DIR)/stellar-core_testnet_legacy.cfg \
             $(TESTDATA_DIR)/stellar-history.testnet.6714239.json $(TESTDATA_DIR)/stellar-history.livenet.15686975.json \
             $(TESTDATA_DIR)/stellar-core_testnet_validator.cfg $(TESTDATA_DIR)/stellar-core_example_validators.cfg \
             $(TESTDATA_DIR)/stellar-history.testnet.6714239.networkPassphrase.json \
             $(TESTDATA_DIR)/stellar-history.testnet.6714239.networkPassphrase.v2.json

BUILT_SOURCES = $(SRC_X_FILES:.x=.h) $(GENERATED_VERSION_SOURCES) $(TEST_FILES)

$(SRC_X_FILES:.x=.h): $(XDRC)
SUFFIXES = .x .h .rs
.x.h:
	mkdir -p $(@D)
	$(XDRC) -hh -pedantic -o $@ $<

BISON=bison
FLEX=flex

util/xdrquery/XDRQueryScanner.cpp: util/xdrquery/XDRQueryScanner.ll
	$(FLEX) --outfile=$@ $<

util/xdrquery/XDRQueryParser.cpp: util/xdrquery/XDRQueryParser.yy
	$(BISON) --defines=util/xdrquery/XDRQueryParser.h  --output=$@ $<

util/xdrquery/XDRQueryParser.h: util/xdrquery/XDRQueryParser.cpp
	touch $@

BUILT_SOURCES += $(GENERATED_XDRQUERY_SOURCES) util/xdrquery/XDRQueryParser.h
stellar_core_SOURCES += $(GENERATED_XDRQUERY_SOURCES) util/xdrquery/XDRQueryParser.h

# Old automakes have buggy dependency tracking for conditional generated
# sources. We work around this here by making rust/RustBridge.{cpp,h} generated
# in all cases, and just empty in the non-rust case. Also because of the way old
# automake relies on horrible path-munging code in config.status to extract the
# names of depfiles from the Makefile itself, we can't use any variables in the
# SOURCES addition we're doing here, have to list unadorned paths.

BUILT_SOURCES += rust/RustBridge.h $(GENERATED_RUST_SOURCES)
stellar_core_SOURCES += rust/RustBridge.h $(GENERATED_RUST_SOURCES)

if UNIFIED_RUST
RUST_TOOLCHAIN_CHANNEL=nightly
else
RUST_TOOLCHAIN_FILE=$(top_srcdir)/rust-toolchain.toml
RUST_TOOLCHAIN_CHANNEL=$(shell sed -n 's/channel *= *"\([^"]*\)"/\1/p' $(RUST_TOOLCHAIN_FILE))
endif
CARGO=cargo +$(RUST_TOOLCHAIN_CHANNEL)

# we pass some configuration by environment variable
# to tests since they can't take command-line arguments.
export RUST_TOOLCHAIN_CHANNEL
export SKIP_SOROBAN_TESTS
export top_srcdir
export top_builddir

RUST_BUILD_DIR=$(top_builddir)/src/rust
RUST_BIN_DIR=$(RUST_BUILD_DIR)/bin
RUST_TARGET_DIR=$(top_builddir)/target
RUST_CXXBRIDGE=$(RUST_BIN_DIR)/cxxbridge
RUST_PROFILE=release

check-rust-profile: Makefile
	@case "$(RUST_PROFILE)" in \
		release|dev) ;; \
		*) echo "Error: RUST_PROFILE must be 'release' or 'dev', got '$(RUST_PROFILE)'" >&2; exit 1;; \
	esac

# Of course, RUST_PROFILE can't be used as an argument directly because cargo
# doesn't let you pass --debug, that's the default! you can only pass --release.
# So we have to derive a new variable here.
RUST_PROFILE_ARG := $(if $(findstring release,$(RUST_PROFILE)),--release,)

# Also for even more nonsense reasons the debug profile name is actually `dev`
# but only in the command line, the target subdirectory gets called `debug`.
RUST_PROFILE_DIR := $(if $(findstring release,$(RUST_PROFILE)),release,debug)

RUST_DEP_TREE_STAMP=$(RUST_BUILD_DIR)/src/dep-trees/equal-trees.stamp
SOROBAN_LIBS_STAMP=$(RUST_BUILD_DIR)/soroban/soroban-libs.stamp
RUST_HOST_DEPFILES=rust/Cargo.toml $(top_srcdir)/Cargo.toml $(top_srcdir)/Cargo.lock $(RUST_DEP_TREE_STAMP)

SOROBAN_BUILD_DIR=$(abspath $(RUST_BUILD_DIR))/soroban

# There's some subtlety to the way the next few lines work and interact with the
# "next protocol" flag `--enable-next-protocol-version-unsafe-for-production`.
#
# There are two possible and legitimate ways to interpret this flag:
#
#    1. If the flag is on, build the submodule named by the next protocol
#       version, with no additional feature-flags passed to cargo. If the flag
#       is off, don't build that submodule at all.
#
#    2. If the flag is on, build the submodule named by the _current_ protocol
#       version but pass the `--features=next` flag to cargo. If the flag is
#       off, don't pass `--features=next` to the submodule.
#
# Both of these interpretations are valid and have their own use cases. They
# happen at different times during the development process and we want to allow
# both. Interpretation #1 is useful while a protocol is in-development and we
# want to be certain that the entire submodule isn't built or shipped by
# accident. Interpretation #2 is useful when a protocol is in-production but we
# still want the "next" builds to be available to experiment with protocol
# changes in the current codebase (or at minimum: to be identical to curr, but
# to pass CI and not spuriously fail to build or run)
#
# If you want interpretation #1, you should put the submodule in the variable
# `WIP_SOROBAN_PROTOCOL`. If you want interpretation #2, you should leave that
# variable empty (and include or exclude submodules from the list of
# ALL_SOROBAN_PROTOCOLS as you see fit).

ALL_SOROBAN_PROTOCOLS=p21 p22 p23 p24 p25
WIP_SOROBAN_PROTOCOL=p26

if ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
ALL_SOROBAN_PROTOCOLS+=$(WIP_SOROBAN_PROTOCOL)
# This means "only pass --features=next if there's no WIP submodule"
SOROBAN_FEATURE_NEXT=$(if $(WIP_SOROBAN_PROTOCOL),,$(CARGO_FEATURE_NEXT))
else
SOROBAN_FEATURE_NEXT=
endif

SOROBAN_MAX_PROTOCOL=$(lastword $(sort $(ALL_SOROBAN_PROTOCOLS)))

# Export the list of protocols to test for the check-sorobans test script
export SOROBAN_PROTOCOLS_TO_TEST=$(ALL_SOROBAN_PROTOCOLS)

define soroban_lib_dir
$(shell printf '$(SOROBAN_BUILD_DIR)/%s/target/$(RUST_PROFILE_DIR)' $(1))
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

rust/RustBridge.h: rust/src/bridge.rs $(SRC_RUST_FILES) Makefile $(RUST_CXXBRIDGE)
	$(RUST_CXXBRIDGE) $< --cfg test=false --header --output $@.tmp
	@if cmp -s $@.tmp $@; then rm -v $@.tmp; else mv -v $@.tmp $@; fi

rust/RustBridge.cpp: rust/src/bridge.rs $(SRC_RUST_FILES) Makefile $(RUST_CXXBRIDGE)
	$(RUST_CXXBRIDGE) $< --cfg test=false --output $@.tmp
	@if cmp -s $@.tmp $@; then rm -v $@.tmp; else mv -v $@.tmp $@; fi

# This is just a convenience target for rebuilding the explicit depfiles we
# check in to this repo as a secondary check on the lockfiles in the soroban
# submodules. You should still carefully inspect them if they change!

$(RUST_DEP_TREE_STAMP): $(wildcard rust/soroban/*/Cargo.*) Makefile $(RUST_TOOLCHAIN_FILE)
	rm -f $@
	mkdir -p $(RUST_BUILD_DIR)/src/dep-trees
	for proto in $(ALL_SOROBAN_PROTOCOLS); \
	do \
		$(CARGO) tree --manifest-path $(top_srcdir)/src/rust/soroban/$${proto}/Cargo.toml --locked --package soroban-env-host --edges no-dev --target all \
		| sed -e "s@$(abspath $(top_srcdir))/@@g" > $(RUST_BUILD_DIR)/src/dep-trees/$${proto}-actual.txt ; \
		if ! diff -u $(top_srcdir)/src/rust/src/dep-trees/$${proto}-expect.txt $(RUST_BUILD_DIR)/src/dep-trees/$${proto}-actual.txt; \
		then \
			echo "dep trees differ, please update $${proto}-expect.txt or roll back submodule"; \
			exit 1; \
		fi; \
	done
	touch $@

# The "unified" rust build is a special non-production mode that builds all of
# the rust dependencies of librust_stellar_core.a through a single cargo
# invocation, which is actually the "normal" way cargo operates, but which also
# has the negative side effect of resolving (merging) different point releases
# and pre-release minor versions across transitive dependencies, which means we
# can't control the _exact_ transitive dependencies as well as we'd like.
#
# So we only use the unified rust build for certain special cases such as
# testing with asan/tsan (they seem to only work well when built this way) and
# use the non-unified build (with separate .a files for each separate soroban
# version) for production builds.

if UNIFIED_RUST

# In the unified build, we have to pass the --target flag. This actually breaks
# the non-unified build, so we wind up setting LIBRUST_STELLAR_CORE separately
# in unified and non-unified builds.
RUST_TARGET=$(shell rustc -vV | sed -n 's/host: //p')
LIBRUST_STELLAR_CORE=$(RUST_TARGET_DIR)/$(RUST_TARGET)/$(RUST_PROFILE_DIR)/librust_stellar_core.a

RUSTFLAGS_ASAN := $(if $(findstring -fsanitize=address,$(CXXFLAGS)),-Zsanitizer=address,)
RUSTFLAGS_TSAN := $(if $(findstring -fsanitize=thread,$(CXXFLAGS)),-Zsanitizer=thread,)
RUSTFLAGS_SANI := $(RUSTFLAGS_ASAN) $(RUSTFLAGS_TSAN)
CARGOFLAGS_BUILDSTD := $(if $(findstring sanitizer,$(RUSTFLAGS_SANI)),-Zbuild-std,)
RUSTFLAGS_CFGS := $(if $(findstring sanitizer,$(RUSTFLAGS_SANI)),--cfg curve25519_dalek_backend=\"serial\",)

$(LIBRUST_STELLAR_CORE): $(RUST_HOST_DEPFILES) $(SRC_RUST_FILES) Makefile $(RUST_TOOLCHAIN_FILE)
	cd $(abspath $(top_srcdir))/src/rust && \
	CC="$(CC)" CXX="$(CXX)" LD="$(LD)" CFLAGS="$(CFLAGS)" CXXFLAGS="$(CXXFLAGS)" CXXSTDLIB="$(CXXSTDLIB)" LDFLAGS="$(LDFLAGS)" \
	RUSTFLAGS="$(RUSTFLAGS_SANI) $(RUSTFLAGS_CFGS)" \
	CARGO_NET_GIT_FETCH_WITH_CLI=true \
	$(CARGO) rustc \
		$(CARGOFLAGS_BUILDSTD) \
		--package stellar-core \
		--target $(RUST_TARGET) \
		--features unified \
		$(RUST_PROFILE_ARG) \
		--locked \
		--target-dir $(abspath $(RUST_TARGET_DIR)) \
		$(CARGO_FEATURE_TRACY) $(CARGO_FEATURE_NEXT) $(CARGO_FEATURE_TESTUTILS) $(CARGO_FEATURE_FUZZ)
	ranlib $@

else # !UNIFIED_RUST

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
				FEATURE_FLAGS="$(CARGO_FEATURE_TRACY) $(SOROBAN_FEATURE_NEXT) $(SOROBAN_MAX_PROTOCOL_TESTUTILS_FLAGS)" \
			;; \
			*) \
				FEATURE_FLAGS="$(CARGO_FEATURE_TRACY)" \
			;; \
		esac ; \
		mkdir -p $(SOROBAN_BUILD_DIR)/$$proto/target && \
		cd $(abspath $(top_srcdir))/src/rust/soroban/$$proto && \
		CC="$(CC)" CXX="$(CXX)" LD="$(LD)" CFLAGS="$(CFLAGS)" CXXFLAGS="$(CXXFLAGS)" CXXSTDLIB="$(CXXSTDLIB)" LDFLAGS="$(LDFLAGS)" \
		RUSTFLAGS="-Cmetadata=$$proto" \
		CARGO_TARGET_DIR=$(SOROBAN_BUILD_DIR)/$$proto/target \
		CARGO_NET_GIT_FETCH_WITH_CLI=true \
		$(CARGO) build \
			--package soroban-env-host \
			$(RUST_PROFILE_ARG) \
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

LIBRUST_STELLAR_CORE=$(RUST_TARGET_DIR)/$(RUST_PROFILE_DIR)/librust_stellar_core.a

$(LIBRUST_STELLAR_CORE): $(RUST_HOST_DEPFILES) $(SRC_RUST_FILES) Makefile $(SOROBAN_LIBS_STAMP) $(RUST_TOOLCHAIN_FILE)
	cd $(abspath $(top_srcdir))/src/rust && \
	CC="$(CC)" CXX="$(CXX)" LD="$(LD)" CFLAGS="$(CFLAGS)" CXXFLAGS="$(CXXFLAGS)" CXXSTDLIB="$(CXXSTDLIB)" LDFLAGS="$(LDFLAGS)" \
	CARGO_NET_GIT_FETCH_WITH_CLI=true \
	$(CARGO) rustc \
		--package stellar-core \
		$(RUST_PROFILE_ARG) \
		--locked \
		--target-dir $(abspath $(RUST_TARGET_DIR)) \
		$(CARGO_FEATURE_TRACY) $(CARGO_FEATURE_NEXT) $(CARGO_FEATURE_TESTUTILS) $(CARGO_FEATURE_FUZZ) \
		-- \
		$(ALL_SOROBAN_EXTERN_ARGS) \
		$(ALL_SOROBAN_DEPEND_ARGS)
	ranlib $@

endif # UNIFIED_RUST

stellar_core_LDADD += $(LIBRUST_STELLAR_CORE) -ldl

$(srcdir)/src.mk: $(top_srcdir)/make-mks
	cd $(top_srcdir) && ./make-mks

$(TESTDATA_DIR)/%.cfg : $(top_srcdir)/docs/%.cfg
	mkdir -p $(@D) && cp $< $@

$(TESTDATA_DIR)/%.json : $(top_srcdir)/src/history/serialize-tests/%.json
	mkdir -p $(@D) && cp $< $@

.PHONY: always
always: check-rust-profile
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
TESTS += test/check-sorobans

format: always
if USE_CLANG_FORMAT
	cd $(srcdir) && $(CLANG_FORMAT) -style=file -i $(SRC_CXX_FILES) $(SRC_H_FILES) $(SRC_TEST_CXX_FILES) $(SRC_TEST_H_FILES)
endif # USE_CLANG_FORMAT
	cd $(srcdir) && $(CARGO) fmt --all

# Unified fuzzing infrastructure (libfuzzer-compatible)
# These targets build separate binaries for each fuzz target that can be
# run with libfuzzer, honggfuzz, or AFL++.
#
# To build: LIB_FUZZING_ENGINE="-fsanitize=fuzzer" ./configure --enable-fuzz && make fuzz-targets
# To run: ./fuzz_tx corpus/tx/
#
# Naming follows oss-fuzz conventions:
#   - Binary: fuzz_<target>
#   - Corpus: fuzz_<target>_seed_corpus.zip
#   - Dictionary: fuzz_<target>.dict
if ENABLE_FUZZ

# ============================================================================
# Fuzz Build Configuration
# ============================================================================

# FuzzMain.cpp is the only file that needs FUZZ_TARGET_NAME, so we compile it
# separately for each target. All other sources go into a shared library.
# SRC_TEST_CXX_FILES already excludes FuzzMain.cpp (see make-mks)
FUZZ_MAIN = test/fuzz/FuzzMain.cpp

# Common sources for the fuzz library (everything except FuzzMain.cpp)
FUZZ_COMMON_SOURCES = \
	$(SRC_CXX_FILES) \
	$(SRC_TEST_CXX_FILES) \
	$(GENERATED_VERSION_SOURCES) \
	$(GENERATED_XDRQUERY_SOURCES) \
	$(GENERATED_RUST_SOURCES)

# Common compiler flags for fuzz targets
FUZZ_CXXFLAGS = $(AM_CPPFLAGS) $(AM_CXXFLAGS) -DFUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION -DBUILD_TESTS=1

# Convenience library containing all common fuzz code (compiled ONCE)
# This avoids recompiling hundreds of .o files for each fuzz target.
noinst_LIBRARIES = libfuzz_common.a
libfuzz_common_a_SOURCES = $(FUZZ_COMMON_SOURCES)
libfuzz_common_a_CXXFLAGS = $(FUZZ_CXXFLAGS)

# Common libraries for linking
FUZZ_LIBS = libfuzz_common.a \
	$(soci_LIBS) $(libmedida_LIBS) \
	$(top_builddir)/lib/lib3rdparty.a $(sqlite3_LIBS) \
	$(libpq_LIBS) $(xdrpp_LIBS) $(libsodium_LIBS) $(libunwind_LIBS) \
	$(LIBRUST_STELLAR_CORE) -ldl

# Link flags for fuzz targets.
# We use --whole-archive to force inclusion of all registration objects from
# libfuzz_common.a (otherwise static constructors for target registration are
# discarded by the linker as "unreferenced")
FUZZ_LINKER_FLAGS = -Wl,--whole-archive,libfuzz_common.a,--no-whole-archive $(LIB_FUZZING_ENGINE)

# ============================================================================
# Fuzz Target Infrastructure
# ============================================================================
#
# To add a new fuzz target:
# 1. Create src/test/fuzz/targets/<Name>FuzzTarget.cpp implementing FuzzTarget
# 2. Add <name> to FUZZ_TARGETS
# 3. Add fuzz_<name> to noinst_PROGRAMS
# 4. Add fuzz_<name>_{SOURCES,CXXFLAGS,LDADD,LDFLAGS} target definition
# 5. Run ./make-mks && ./autogen.sh && ./configure --enable-fuzz
#
# The automake rules will automatically:
# - Build fuzz_<name> binary
# - Create seed corpus generation target
# - Create regression test target

# List of fuzz targets (lowercase names)
FUZZ_TARGETS = tx overlay soroban_expr soroban_wasmi

# Fuzz binaries are noinst because they're development tools
noinst_PROGRAMS = fuzz_tx fuzz_overlay fuzz_soroban_expr fuzz_soroban_wasmi

# Each fuzz target only compiles FuzzMain.cpp (with its specific -DFUZZ_TARGET_NAME)
# and links against the shared libfuzz_common.a library.
fuzz_tx_SOURCES = $(FUZZ_MAIN)
fuzz_tx_CXXFLAGS = $(FUZZ_CXXFLAGS) -DFUZZ_TARGET_NAME=\"tx\"
fuzz_tx_LDADD = $(FUZZ_LIBS)
fuzz_tx_LDFLAGS = $(FUZZ_LINKER_FLAGS)

fuzz_overlay_SOURCES = $(FUZZ_MAIN)
fuzz_overlay_CXXFLAGS = $(FUZZ_CXXFLAGS) -DFUZZ_TARGET_NAME=\"overlay\"
fuzz_overlay_LDADD = $(FUZZ_LIBS)
fuzz_overlay_LDFLAGS = $(FUZZ_LINKER_FLAGS)

# Soroban fuzz targets (require --features fuzz in Rust build)
fuzz_soroban_expr_SOURCES = $(FUZZ_MAIN)
fuzz_soroban_expr_CXXFLAGS = $(FUZZ_CXXFLAGS) -DFUZZ_TARGET_NAME=\"soroban_expr\"
fuzz_soroban_expr_LDADD = $(FUZZ_LIBS)
fuzz_soroban_expr_LDFLAGS = $(FUZZ_LINKER_FLAGS)

fuzz_soroban_wasmi_SOURCES = $(FUZZ_MAIN)
fuzz_soroban_wasmi_CXXFLAGS = $(FUZZ_CXXFLAGS) -DFUZZ_TARGET_NAME=\"soroban_wasmi\"
fuzz_soroban_wasmi_LDADD = $(FUZZ_LIBS)
fuzz_soroban_wasmi_LDFLAGS = $(FUZZ_LINKER_FLAGS)

# ============================================================================
# Phony Targets for Fuzzing Workflow
# ============================================================================

# Build all fuzz targets
fuzz-targets: $(noinst_PROGRAMS)
	@echo "Built fuzz targets: $(FUZZ_TARGETS)"

# Generate seed corpus for all targets
fuzz-corpus: stellar-core
	@for target in $(FUZZ_TARGETS); do \
		mkdir -p corpus/$$target; \
		echo "Generating $$target seed corpus..."; \
		./stellar-core gen-fuzz --target=$$target --output-dir=corpus/$$target 2>/dev/null; \
		echo "Generated seed files in corpus/$$target/"; \
	done

# Run regression tests on all targets
fuzz-check: stellar-core
	./stellar-core test [fuzz]

# Package seed corpus as zip files (oss-fuzz convention)
fuzz-corpus-zip: fuzz-corpus
	@for target in $(FUZZ_TARGETS); do \
		if [ -d corpus/$$target ]; then \
			cd corpus && zip -q -r ../fuzz_$${target}_seed_corpus.zip $$target && cd ..; \
			echo "Created fuzz_$${target}_seed_corpus.zip"; \
		fi; \
	done

# Install fuzz targets to $OUT (for oss-fuzz build.sh)
fuzz-install:
	@if [ -z "$(OUT)" ]; then \
		echo "Error: OUT not set. This target is for oss-fuzz builds."; \
		exit 1; \
	fi
	@for target in $(FUZZ_TARGETS); do \
		cp fuzz_$$target $(OUT)/fuzz_$$target; \
		echo "Installed fuzz_$$target to $(OUT)"; \
		if [ -f fuzz_$${target}_seed_corpus.zip ]; then \
			cp fuzz_$${target}_seed_corpus.zip $(OUT)/; \
		fi; \
	done

.PHONY: fuzz-targets fuzz-corpus fuzz-check fuzz-corpus-zip fuzz-install

endif # ENABLE_FUZZ

clean-local:
	rm -rf $(top_builddir)/target $(top_builddir)/src/rust/soroban/*/target $(RUST_DEP_TREE_STAMP) $(SOROBAN_LIBS_STAMP)

CLEANFILES = $(BUILT_SOURCES) *~ */*~ stellar*.log
MAINTAINERCLEANFILES = $(srcdir)/Makefile.in $(srcdir)/*~ $(srcdir)/*/*~

EXTRA_DIST = $(SRC_X_FILES) test/selftest-nopg test/selftest-pg
