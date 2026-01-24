// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "rust/RustBridge.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace stellar
{

/**
 * FuzzTarget is the interface all fuzz targets must implement.
 *
 * This design follows the libfuzzer model where each target is a function
 * that accepts a byte buffer and returns 0 on success. The infrastructure
 * handles setup/teardown, corpus management, and fuzzer integration.
 */
class FuzzTarget
{
  public:
    virtual ~FuzzTarget() = default;

    // Name of this fuzz target (e.g., "tx", "overlay")
    virtual std::string name() const = 0;

    // Human-readable description
    virtual std::string description() const = 0;

    // Initialize the target (called once before fuzzing starts)
    virtual void initialize() = 0;

    // Run one fuzzing iteration with the given input
    // Returns FUZZ_SUCCESS on success (input was processed)
    // Throws on catastrophic error
    virtual FuzzResultCode run(uint8_t const* data, size_t size) = 0;

    // Shutdown the target (called once when fuzzing is done)
    virtual void shutdown() = 0;

    // Maximum input size this target can handle
    virtual size_t maxInputSize() const = 0;

    // Generate a single seed input as a byte buffer.
    // This is the core generation logic that each target must implement.
    virtual std::vector<uint8_t> generateSeedInput() = 0;

    // Generate seed corpus entries and write them to the given directory.
    // Default implementation calls generateSeedInput() for each entry.
    virtual void generateSeedCorpus(std::string const& outputDir, size_t count);
};

/**
 * FuzzTargetRegistry manages all registered fuzz targets.
 *
 * Targets self-register using the REGISTER_FUZZ_TARGET macro.
 */
class FuzzTargetRegistry
{
  public:
    using TargetFactory = std::function<std::unique_ptr<FuzzTarget>()>;

    struct TargetInfo
    {
        std::string name;
        std::string description;
        TargetFactory factory;
    };

    // Get the global registry instance
    static FuzzTargetRegistry& instance();

    // Register a fuzz target factory
    void registerTarget(std::string const& name, std::string const& description,
                        TargetFactory factory);

    // Get list of all registered targets
    std::vector<TargetInfo> const& targets() const;

    // Find a target by name
    std::optional<TargetInfo> findTarget(std::string const& name) const;

    // Create a target instance by name
    std::unique_ptr<FuzzTarget> createTarget(std::string const& name) const;

    // List all target names
    std::vector<std::string> listTargetNames() const;

  private:
    FuzzTargetRegistry() = default;
    std::vector<TargetInfo> mTargets;
};

/**
 * FuzzTargetRegistrar is a helper class for static registration.
 */
class FuzzTargetRegistrar
{
  public:
    FuzzTargetRegistrar(std::string const& name, std::string const& description,
                        FuzzTargetRegistry::TargetFactory factory);
};

// ============================================================================
// Shared test helpers for fuzz target regression/smoke tests
// ============================================================================

// Run a smoke test: create target, generate input, run it
void runFuzzTargetSmokeTest(std::string const& targetName);

// Run corpus regression: find corpus dir, run all files through target
void runFuzzTargetCorpusRegression(std::string const& targetName);

// ============================================================================
// REGISTER_FUZZ_TARGET macro
// ============================================================================
//
// This macro:
// 1. Registers the fuzz target with the FuzzTargetRegistry
// 2. Generates Catch2 TEST_CASEs for smoke and corpus regression testing
//
// Usage:
//   REGISTER_FUZZ_TARGET(MyTarget, "my-target", "Description of my target");
//
// Where MyTarget is a class derived from FuzzTarget.
//
// This generates two tests tagged "[fuzz][my-target]":
// - "fuzz my-target smoke" - runs a single generated input
// - "fuzz my-target corpus" - runs all corpus files (if present)

#define REGISTER_FUZZ_TARGET(TargetClass, targetName, description) \
    static stellar::FuzzTargetRegistrar gFuzzRegistrar_##TargetClass( \
        targetName, description, \
        []() -> std::unique_ptr<stellar::FuzzTarget> { \
            return std::make_unique<TargetClass>(); \
        }); \
    TEST_CASE("fuzz " targetName " smoke", "[fuzz][" targetName "][smoke]") \
    { \
        stellar::runFuzzTargetSmokeTest(targetName); \
    } \
    TEST_CASE("fuzz " targetName " corpus", "[fuzz][" targetName "][corpus]") \
    { \
        stellar::runFuzzTargetCorpusRegression(targetName); \
    }

} // namespace stellar
