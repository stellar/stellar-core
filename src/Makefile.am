## Process this file with automake to produce Makefile.in

AUTOMAKE_OPTIONS = serial-tests
bin_PROGRAMS = stellar-core

include $(top_srcdir)/common.mk
include $(srcdir)/src.mk

noinst_HEADERS = $(SRC_H_FILES)

if BUILD_TESTS
stellar_core_SOURCES = main/StellarCoreVersion.cpp main/XDRFilesSha256.cpp $(SRC_CXX_FILES) $(SRC_TEST_CXX_FILES)
else # !BUILD_TESTS
stellar_core_SOURCES = main/StellarCoreVersion.cpp main/XDRFilesSha256.cpp $(SRC_CXX_FILES)
endif # !BUILD_TESTS

if USE_TRACY
CARGO_FEATURE_TRACY = --features tracy
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

RUST_BUILD_DIR=$(top_builddir)/src/rust
RUST_BIN_DIR=$(RUST_BUILD_DIR)/bin
RUST_TARGET_DIR=$(top_builddir)/target
RUST_CXXBRIDGE=$(RUST_BIN_DIR)/cxxbridge
RUST_PROFILE=release
RUST_DEP_TREE_STAMP=$(RUST_BUILD_DIR)/src/dep-trees/equal-trees.stamp
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

$(RUST_CXXBRIDGE):
	mkdir -p $(RUST_BIN_DIR)
	CARGO_HTTP_MULTIPLEXING=false $(CARGO) install --locked --root $(RUST_BUILD_DIR) cxxbridge-cmd --version 1.0.68

rust/RustBridge.h: rust/src/lib.rs $(SRC_RUST_FILES) Makefile $(RUST_CXXBRIDGE)
	$(RUST_CXXBRIDGE) $< --cfg test=false --header --output $@.tmp
	if cmp -s $@.tmp $@; then rm -v $@.tmp; else mv -v $@.tmp $@; fi

rust/RustBridge.cpp: rust/src/lib.rs $(SRC_RUST_FILES) Makefile $(RUST_CXXBRIDGE)
	$(RUST_CXXBRIDGE) $< --cfg test=false --output $@.tmp
	if cmp -s $@.tmp $@; then rm -v $@.tmp; else mv -v $@.tmp $@; fi

# This is just a convenience target for rebuilding the explicit depfiles we
# check in to this repo as a secondary check on the lockfiles in the soroban
# submodules. You should still carefully inspect them if they change!

ALL_RUST_EXPECT_DEPTREES=$(foreach proto,$(ALL_SOROBAN_PROTOCOLS),rust/src/dep-trees/$(proto)-expect.txt)
ALL_RUST_ACTUAL_DEPTREES=$(foreach proto,$(ALL_SOROBAN_PROTOCOLS),$(RUST_BUILD_DIR)/src/dep-trees/$(proto)-actual.txt)

$(RUST_DEP_TREE_STAMP): $(ALL_RUST_EXPECT_DEPTREES) $(ALL_RUST_ACTUAL_DEPTREES) Makefile
	rm -f $@
	for proto in $(ALL_SOROBAN_PROTOCOLS); \
	do \
		if ! diff -u rust/src/dep-trees/$${proto}-expect.txt $(RUST_BUILD_DIR)/src/dep-trees/$${proto}-actual.txt; \
		then \
			echo "dep trees differ, please update $${proto}-expect.txt or roll back submodule"; \
			exit 1; \
		fi; \
	done
	touch $@

# We sequentialize these calculations to avoid running two rustups at once.
# We also use a group-target here (&:) to avoid running the rule once-for-each-target.
$(ALL_RUST_ACTUAL_DEPTREES) &: $(wildcard rust/soroban/*/Cargo.*) Makefile
	for proto in $(ALL_SOROBAN_PROTOCOLS); \
	do \
		cargo tree --manifest-path rust/soroban/$$proto/Cargo.toml --locked --package soroban-env-host --edges no-dev \
		| sed -e "s@$(abspath $(top_srcdir))/@@g" > $(RUST_BUILD_DIR)/src/dep-trees/$$proto-actual.txt ;\
	done

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
# We also have to be somewhat selective about the versions we pass the
# `next` feature to (only the most recent soroban) and the versions we
# pass the `tracy` feature to (only those post-p22 versions that support
# the tracy delayed-init feature).
#
# We also have to sequentialize the builds of the sorobans after the
# dep-tree-stamp files and before the stellar-core build, because rustup
# gets invoked and it is not concurrency-safe. We also use a group-target
# here (&:) to avoid running the rule once-for-each-target.
$(ALL_SOROBAN_LIBS) &: $(wildcard rust/soroban/p*/Cargo.lock) Makefile $(RUST_DEP_TREE_STAMP)
	for proto in $(ALL_SOROBAN_PROTOCOLS) ; \
	do \
		FEATURE_FLAGS="" ; \
		case "$$proto" in \
			p21) \
				FEATURE_FLAGS="$(CARGO_FEATURE_TESTUTILS)" \
			;; \
			$(SOROBAN_MAX_PROTOCOL)) \
				FEATURE_FLAGS="$(CARGO_FEATURE_TRACY) $(CARGO_FEATURE_NEXT) $(CARGO_FEATURE_TESTUTILS)" \
			;; \
			*) \
				FEATURE_FLAGS="$(CARGO_FEATURE_TRACY) $(CARGO_FEATURE_TESTUTILS)" \
			;; \
		esac ; \
		cd $(abspath $(RUST_BUILD_DIR))/soroban/$$proto && \
		RUSTFLAGS=-Cmetadata=$$proto \
		CARGO_NET_GIT_FETCH_WITH_CLI=true cargo build \
			--package soroban-env-host \
			--$(RUST_PROFILE) \
			--locked \
			$$FEATURE_FLAGS \
		|| exit 1; \
	done

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
$(LIBRUST_STELLAR_CORE): $(RUST_HOST_DEPFILES) $(SRC_RUST_FILES) Makefile $(ALL_SOROBAN_LIBS)
	CARGO_NET_GIT_FETCH_WITH_CLI=true cargo rustc \
		--package stellar-core \
		--$(RUST_PROFILE) \
		--locked \
		--target-dir $(abspath $(RUST_TARGET_DIR)) \
		$(CARGO_FEATURE_TRACY) $(CARGO_FEATURE_NEXT) \
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
	rm -rf $(top_builddir)/target $(top_builddir)/src/rust/soroban/*/target

CLEANFILES = $(BUILT_SOURCES) *~ */*~ stellar*.log
MAINTAINERCLEANFILES = $(srcdir)/Makefile.in $(srcdir)/*~ $(srcdir)/*/*~

EXTRA_DIST = $(SRC_X_FILES) test/selftest-nopg test/selftest-pg
