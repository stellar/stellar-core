// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

/**
 * SorobanFuzzTargets.h - Soroban fuzzing targets
 *
 * This file provides a generic C++ wrapper for Soroban fuzz targets implemented
 * in Rust. All Soroban targets share the same interface from stellar-core's
 * side: they just pass their name and input data to the Rust
 * run_soroban_fuzz_target() function.
 *
 * See SorobanFuzzTargets.cpp for the current list of registered targets and
 * their descriptions.
 *
 * The actual fuzz target implementations live in soroban-env-host/src/fuzz.rs
 * in the soroban repo (behind the `fuzz` feature). This file just provides
 * the C++ bridge.
 *
 * These targets require the fuzz feature to be enabled in the Rust build
 * (--features fuzz).
 *
 * To add a new Soroban fuzz target:
 * 1. Add the target function to soroban-env-host/src/fuzz.rs in the soroban
 * repo
 * 2. Add a match arm in stellar-core's soroban_fuzz.rs
 * run_soroban_fuzz_target()
 * 3. Add REGISTER_SOROBAN_FUZZ_TARGET(target_name, "desc") in
 * SorobanFuzzTargets.cpp
 * 4. Add fuzz_target_name entries to Makefile.am
 */

#include "test/fuzz/FuzzTargetRegistry.h"
#include <string>

namespace stellar
{

/**
 * SorobanFuzzTarget - Generic Soroban fuzz target wrapper
 *
 * This class wraps any Soroban fuzz target implemented in Rust. The target
 * name is passed to the constructor and used to dispatch to the appropriate
 * Rust implementation via run_soroban_fuzz_target().
 *
 * All Soroban targets share the same interface:
 * - Input bytes are passed to Rust
 * - Rust maps them to some datatype(s) typically using the arbitrary crate
 * - Rust runs the target-specific logic (expr evaluation, wasm execution, etc.)
 * - Return value indicates success (0), unknown target (-1), or insufficient
 * data (-2)
 * - Internal errors panic (which libfuzzer catches as crashes)
 */
class SorobanFuzzTarget : public FuzzTarget
{
  public:
    explicit SorobanFuzzTarget(std::string targetName,
                               std::string targetDescription);

    std::string name() const override;
    std::string description() const override;
    void initialize() override;
    FuzzResultCode run(uint8_t const* data, size_t size) override;
    void shutdown() override;
    size_t maxInputSize() const override;
    std::vector<uint8_t> generateSeedInput() override;

  private:
    std::string mTargetName;
    std::string mTargetDescription;
};

// ============================================================================
// REGISTER_SOROBAN_FUZZ_TARGET macro
// ============================================================================
//
// This macro:
// 1. Registers the Soroban fuzz target with the FuzzTargetRegistry
// 2. Generates Catch2 TEST_CASEs for smoke and corpus regression testing
//
// Usage:
//   REGISTER_SOROBAN_FUZZ_TARGET(soroban_expr, "Description");
//
// The target name becomes "soroban_expr" (using the C++ identifier).
// This generates two tests tagged "[fuzz][soroban_expr]":
// - "fuzz soroban_expr smoke"
// - "fuzz soroban_expr corpus"

#define REGISTER_SOROBAN_FUZZ_TARGET(cppName, desc) \
    static struct SorobanFuzzTargetRegistrar_##cppName \
    { \
        SorobanFuzzTargetRegistrar_##cppName() \
        { \
            FuzzTargetRegistry::instance().registerTarget( \
                #cppName, desc, []() -> std::unique_ptr<FuzzTarget> { \
                    return std::make_unique<SorobanFuzzTarget>(#cppName, \
                                                               desc); \
                }); \
        } \
    } gSorobanFuzzTargetRegistrar_##cppName; \
    TEST_CASE("fuzz " #cppName " smoke", "[fuzz][" #cppName "][smoke]") \
    { \
        stellar::runFuzzTargetSmokeTest(#cppName); \
    } \
    TEST_CASE("fuzz " #cppName " corpus", "[fuzz][" #cppName "][corpus]") \
    { \
        stellar::runFuzzTargetCorpusRegression(#cppName); \
    }

} // namespace stellar
