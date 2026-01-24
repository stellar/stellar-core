// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "test/fuzz/targets/SorobanFuzzTargets.h"

#include "rust/RustBridge.h"
#include "test/Catch2.h"
#include "util/Logging.h"

#include <fstream>
#include <random>

namespace stellar
{

// ============================================================================
// SorobanFuzzTarget implementation
// ============================================================================

SorobanFuzzTarget::SorobanFuzzTarget(std::string targetName,
                                     std::string targetDescription)
    : mTargetName(std::move(targetName))
    , mTargetDescription(std::move(targetDescription))
{
}

std::string
SorobanFuzzTarget::name() const
{
    return mTargetName;
}

std::string
SorobanFuzzTarget::description() const
{
    return mTargetDescription;
}

void
SorobanFuzzTarget::initialize()
{
    // The Rust side handles all initialization
    LOG_INFO(DEFAULT_LOG, "Initializing Soroban fuzz target: {}", mTargetName);
}

FuzzResultCode
SorobanFuzzTarget::run(uint8_t const* data, size_t size)
{
    // Call into Rust to run the fuzz target
    // The function returns:
    //   0: success
    //  -1: unknown target (shouldn't happen)
    //  -2: rejected input (normal for fuzzing)
    rust::Slice<const uint8_t> slice(data, size);
    return stellar::rust_bridge::run_soroban_fuzz_target(mTargetName, slice);
}

void
SorobanFuzzTarget::shutdown()
{
    // Nothing to clean up - Rust handles its own resources
}

size_t
SorobanFuzzTarget::maxInputSize() const
{
    // All Soroban fuzz targets use the arbitrary crate to deserialize
    // fuzz input into structured test cases. 64KB is a reasonable limit
    // that allows complex inputs while keeping execution times manageable.
    return 64 * 1024;
}

// Named constants for seed corpus generation
constexpr size_t MIN_SEED_SIZE = 100;
constexpr size_t MAX_SEED_SIZE = 1100; // MIN_SEED_SIZE + range of 1000

std::vector<uint8_t>
SorobanFuzzTarget::generateSeedInput()
{
    // Generate random bytes as seed corpus. The arbitrary crate will
    // structure these into valid test cases.
    // TODO: A future improvement could generate structured seeds in Rust
    // and expose them through the bridge for better initial coverage.
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);

    // Generate random bytes of varying sizes within our bounds
    size_t size = MIN_SEED_SIZE + (gen() % (MAX_SEED_SIZE - MIN_SEED_SIZE));
    std::vector<uint8_t> data(size);
    for (auto& b : data)
    {
        b = static_cast<uint8_t>(dis(gen));
    }
    return data;
}

// ============================================================================
// Register Soroban fuzz targets
// ============================================================================

// Each macro invocation registers a target with the FuzzTargetRegistry.
// The target name must match what's registered in Rust's soroban_fuzz.rs.

REGISTER_SOROBAN_FUZZ_TARGET(soroban_expr,
                             "Fuzz Soroban host with synthetic expressions");

REGISTER_SOROBAN_FUZZ_TARGET(soroban_wasmi,
                             "Fuzz the wasmi VM with arbitrary wasm modules");

} // namespace stellar
