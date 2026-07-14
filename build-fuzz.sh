#!/bin/bash
# Copyright 2026 Stellar Development Foundation and contributors. Licensed
# under the Apache License, Version 2.0. See the COPYING file at the root
# of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

# Build script for oss-fuzz integration.
#
# This script is copy of the "build.sh" integration script
# that OSS-fuzz uses to build fuzz targets for running on their infrastructure
# very slightly modified to work with our CI (using ccache + cached build dirs)

SRC_DIR="$(pwd)"
OUT="${OUT:-$SRC_DIR/fuzz-out}"
if [[ "$OUT" != /* ]]; then
    OUT="$SRC_DIR/$OUT"
fi
mkdir -p "$OUT"

mkdir -p "build-clang-libfuzzer"
cd "build-clang-libfuzzer"

#### ccache config
export CCACHE_DIR="$(pwd)/.ccache"
export CCACHE_COMPRESS=true
export CCACHE_COMPRESSLEVEL=9
# cache size should be large enough for a full build
export CCACHE_MAXSIZE=800M
export CCACHE_CPP2=true

# periodically check to see if caches are old and purge them if so
CACHE_MAX_DAYS=30
if [ -d "${CCACHE_DIR}" ] ; then
    if [ -n "$(find ${CCACHE_DIR} -mtime +$CACHE_MAX_DAYS -print -quit)" ] ; then
        echo Purging old cache dirs "${CCACHE_DIR}" ./target "${HOME}/.cargo/registry" "${HOME}/.cargo/git"
        rm -rf "${CCACHE_DIR}" ./target "${HOME}/.cargo/registry" "${HOME}/.cargo/git"
    fi
fi

ccache -p
ccache -s
ccache -z

. "${HOME}/.cargo/env"
(cd "${SRC_DIR}" && ./autogen.sh)

# NB: the oss-fuzz driver injects sanitizer flags to CFLAGS, CXXFLAGS
# and RUSTFLAGS. This overlaps with our own support for sanitizers,
# but not fatally. It does require us to enable the unified rust
# build.
"${SRC_DIR}/configure" --enable-fuzz --enable-unified-rust-unsafe-for-production --disable-postgres --enable-ccache
make -j $(nproc)
make -C src fuzz-targets
cp src/fuzz_* "${OUT}"
