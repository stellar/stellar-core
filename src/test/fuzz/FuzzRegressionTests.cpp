// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

/**
 * FuzzRegressionTests.cpp - Shared helpers for fuzz target testing
 *
 * This file provides the implementation of shared test helpers used by
 * the REGISTER_FUZZ_TARGET and REGISTER_SOROBAN_FUZZ_TARGET macros.
 *
 * Each macro generates a TEST_CASE that calls these helpers. This file
 * does NOT need to be modified when adding new fuzz targets - just use
 * the REGISTER_*_FUZZ_TARGET macro in your target's .cpp file.
 *
 * The helpers implement:
 * - Smoke testing: Generate random input and run it through the target
 * - Corpus regression: Run all files in corpus/<target>/ through the target
 *
 * To run all fuzz tests:
 *   stellar-core test "[fuzz]"
 *
 * To run tests for a specific target:
 *   stellar-core test "[fuzz][tx]"
 *   stellar-core test "[fuzz][overlay]"
 *   stellar-core test "[fuzz][soroban_expr]"
 */

#include "test/fuzz/FuzzTargetRegistry.h"
#include "util/Logging.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace stellar
{

// Maximum number of iterations to attempt before declaring the smoke test
// failed. This gives the fuzzer enough attempts to generate at least one
// accepted input.
static constexpr size_t SMOKE_TEST_MAX_ITERATIONS = 5000;

// Number of successes we want to see in smoke testing. We should get this
// many before we hit the max iterations.
static constexpr size_t SMOKE_TEST_SUCCESS_TARGET = 10;

void
runFuzzTargetSmokeTest(std::string const& targetName)
{
    size_t successCount = 0;
    auto target = FuzzTargetRegistry::instance().createTarget(targetName);
    target->initialize();

    for (size_t iteration = 0; iteration < SMOKE_TEST_MAX_ITERATIONS;
         ++iteration)
    {
        // Generate a seed input
        auto input = target->generateSeedInput();

        // Run the input through the target
        FuzzResultCode result = target->run(input.data(), input.size());

        switch (result)
        {
        case FuzzResultCode::FUZZ_SUCCESS:
            successCount++;
            if (successCount >= SMOKE_TEST_SUCCESS_TARGET)
            {
                CLOG_INFO(Test,
                          "Smoke test for '{}' succeeded {} times after {} "
                          "iterations",
                          targetName, successCount, iteration + 1);
                target->shutdown();
                return;
            }
            break;

        case FuzzResultCode::FUZZ_REJECTED:
            CLOG_DEBUG(Test,
                       "Smoke test input for '{}' was rejected - continuing",
                       targetName);
            break;

        case FuzzResultCode::FUZZ_DISABLED:
            // Target is not available in this build configuration
            // (e.g., Soroban fuzz targets when Rust 'fuzz' feature is disabled)
            CLOG_WARNING(Test,
                         "Smoke test for '{}' skipped: target is disabled in "
                         "this build",
                         targetName);
            target->shutdown();
            return;

        case FuzzResultCode::FUZZ_TARGET_UNKNOWN:
            throw std::runtime_error(fmt::format(
                "Smoke test for '{}' failed with unknown target error",
                targetName));
        }
    }
    target->shutdown();

    throw std::runtime_error("Smoke test for '" + targetName +
                             "' failed: no accepted inputs after " +
                             std::to_string(SMOKE_TEST_MAX_ITERATIONS) +
                             " iterations");
}

void
runFuzzTargetCorpusRegression(std::string const& targetName)
{
    // Look for corpus in multiple locations
    std::vector<std::string> corpusDirs = {"corpus/" + targetName,
                                           "src/corpus/" + targetName,
                                           "../corpus/" + targetName};

    std::string foundDir;
    for (auto const& dir : corpusDirs)
    {
        if (std::filesystem::exists(dir))
        {
            foundDir = dir;
            break;
        }
    }

    if (foundDir.empty())
    {
        CLOG_WARNING(Tx,
                     "No corpus found for '{}'. Generate with: stellar-core "
                     "gen-fuzz --target={} --output-dir=corpus/{}",
                     targetName, targetName, targetName);
        return;
    }

    auto target = FuzzTargetRegistry::instance().createTarget(targetName);
    target->initialize();

    int filesProcessed = 0;
    for (auto const& entry : std::filesystem::directory_iterator(foundDir))
    {
        if (!entry.is_regular_file())
            continue;

        auto const& path = entry.path();
        // Process .xdr and .bin files (common corpus formats)
        auto ext = path.extension().string();
        if (ext != ".xdr" && ext != ".bin")
            continue;

        CLOG_DEBUG(Tx, "Running fuzz input: {}", path.string());

        try
        {
            std::ifstream in(path, std::ios::binary);
            std::vector<uint8_t> data(target->maxInputSize());
            in.read(reinterpret_cast<char*>(data.data()), data.size());
            auto actual = in.gcount();
            if (actual > 0)
            {
                data.resize(actual);
                target->run(data.data(), data.size());
            }
            filesProcessed++;
        }
        catch (std::exception const& e)
        {
            // Log but don't fail - fuzzer inputs may trigger expected
            // errors
            CLOG_DEBUG(Tx, "Fuzz input {} threw: {}", path.string(), e.what());
            filesProcessed++;
        }
    }

    target->shutdown();

    CLOG_INFO(Tx, "Processed {} corpus files from {} for target '{}'",
              filesProcessed, foundDir, targetName);
}

} // namespace stellar
