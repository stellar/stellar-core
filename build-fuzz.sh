#!/bin/bash
# Copyright 2026 Stellar Development Foundation and contributors. Licensed
# under the Apache License, Version 2.0. See the COPYING file at the root
# of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

# Build script for oss-fuzz integration.
#
# This script builds fuzz targets for use with libfuzzer, honggfuzz, or AFL++.
# It follows the oss-fuzz build conventions:
# - Uses environment variables: $CC, $CXX, $CFLAGS, $CXXFLAGS, $LIB_FUZZING_ENGINE, $OUT
# - Produces binaries in $OUT
# - Creates seed corpus archives as fuzz_<target>_seed_corpus.zip
#
# For local testing with libfuzzer:
#   export CC=clang
#   export CXX=clang++
#   export CFLAGS="-g -fsanitize=fuzzer-no-link,address"
#   export CXXFLAGS="-g -fsanitize=fuzzer-no-link,address"
#   export LIB_FUZZING_ENGINE="-fsanitize=fuzzer"
#   export OUT=./fuzz-out
#   ./build-fuzz.sh
#
# For honggfuzz (local):
#   export CC=hfuzz-clang
#   export CXX=hfuzz-clang++
#   export LIB_FUZZING_ENGINE=""  # honggfuzz provides its own
#   export OUT=./fuzz-out
#   ./build-fuzz.sh
#
# Outputs:
#   $OUT/fuzz_tx                    - Transaction application fuzzer
#   $OUT/fuzz_overlay               - Overlay message fuzzer
#   $OUT/fuzz_tx_seed_corpus.zip    - Seed corpus for tx fuzzer
#   $OUT/fuzz_overlay_seed_corpus.zip - Seed corpus for overlay fuzzer

set -eux

# Get source directory (where this script lives)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${SRC:-$SCRIPT_DIR}"

# Create output directory
: "${OUT:=./fuzz-out}"
mkdir -p "$OUT"

# Change to source directory
cd "$SRC"

# Run autogen if configure doesn't exist
if [ ! -f configure ]; then
    ./autogen.sh
fi

# Configure with fuzzing support
# The --enable-fuzz flag:
#   - Adds FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION define
#   - Sets FUZZING_LIBS from LIB_FUZZING_ENGINE
#   - Enables building of fuzz_* targets
./configure \
    --enable-fuzz \
    --disable-tests \
    --without-postgres

# Build fuzz targets using automake rules
# This builds fuzz_tx, fuzz_overlay, and Soroban targets with proper FUZZ_TARGET_NAME defines
make -j"$(nproc)" fuzz-targets

# Install fuzz targets to $OUT
# Install all fuzz targets (C++ and Soroban)
for target in tx overlay soroban_expr soroban_wasmi; do
    if [ -f "src/fuzz_$target" ]; then
        cp "src/fuzz_$target" "$OUT/"
    fi
done

# Generate and package seed corpus (if stellar-core supports it)
# For now, create empty corpus directories
mkdir -p corpus/tx corpus/overlay corpus/soroban_expr corpus/soroban_wasmi

# Create corpus archives (even if empty, oss-fuzz expects them)
# Create corpus archives for all targets (even if empty, oss-fuzz expects them)
for target in tx overlay soroban_expr soroban_wasmi; do
    if [ -d "corpus/$target" ] && [ "$(ls -A "corpus/$target" 2>/dev/null)" ]; then
        (cd "corpus/$target" && zip -q "$OUT/fuzz_${target}_seed_corpus.zip" *)
    else
        # Create empty zip if no corpus
        touch empty && zip -q "$OUT/fuzz_${target}_seed_corpus.zip" empty && rm empty
    fi
done

echo "Build complete. Fuzz targets in $OUT:"
ls -la "$OUT"
