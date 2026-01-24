// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

/**
 * FuzzMain.cpp - libfuzzer compatible (though not necessarily always driven
 * by libfuzzer) entry point for unified fuzzing infrastructure
 *
 * This file provides the LLVMFuzzerTestOneInput function required by libfuzzer,
 * honggfuzz, and AFL++ (persistent mode). It can also be used with other
 * coverage-guided fuzzers that support this API.
 *
 * The target to fuzz is selected at compile time via FUZZ_TARGET_NAME define.
 * Build system generates separate binaries for each target defined in
 * FUZZ_TARGETS in Makefile.am. See 'make fuzz-targets' for the full list.
 *
 * Usage:
 *
 *   ./fuzz_tx corpus/tx/
 *
 * or just
 *
 *   ./fuzz_tx
 *
 * The fuzzer driver is literally linked into this program, so just run it. The
 * fuzzer driver provides its own main(). If you want to re-run a specific fuzz
 * input outside of the fuzzer, say with logging enabled or such, use
 * stellar-core's 'fuzz-one' command instead.
 */

#ifndef FUZZ_TARGET_NAME
#error "FUZZ_TARGET_NAME must be defined at compile time (e.g., -DFUZZ_TARGET_NAME=\"tx\")"
#endif

#include "test/fuzz/FuzzTargetRegistry.h"
#include "util/Logging.h"
#include "util/Math.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>

namespace
{

// Global state for the fuzz target
std::unique_ptr<stellar::FuzzTarget> gFuzzTarget;
bool gInitialized = false;

void
initializeFuzzer()
{
    if (gInitialized)
    {
        return;
    }

    // Suppress most logging during fuzzing for performance
    stellar::Logging::setLogLevel(stellar::LogLevel::LVL_FATAL, nullptr);

    // Initialize global state needed by stellar-core
    stellar::reinitializeAllGlobalStateWithSeed(1);

    // Create and initialize the target (selected at compile time)
    constexpr const char* targetName = FUZZ_TARGET_NAME;
    gFuzzTarget =
        stellar::FuzzTargetRegistry::instance().createTarget(targetName);

    if (!gFuzzTarget)
    {
        std::fprintf(stderr, "Unknown fuzz target: %s\n", targetName);
        std::fprintf(stderr, "Available targets:\n");
        for (auto const& name :
             stellar::FuzzTargetRegistry::instance().listTargetNames())
        {
            std::fprintf(stderr, "  %s\n", name.c_str());
        }
        std::abort();
    }

    gFuzzTarget->initialize();
    gInitialized = true;
}

} // namespace

/**
 * LLVMFuzzerTestOneInput - standard libfuzzer entry point
 *
 * This function is called by the fuzzer for each input. It should:
 * - Process the input quickly (no I/O, minimal allocation)
 * - Not call exit() or abort() for expected failures
 * - Return 0 on success, -1 on rejected input
 *
 * Crashes and assertion failures will be reported by the fuzzer.
 */
extern "C" int
LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    // Initialize on first call
    if (!gInitialized)
    {
        initializeFuzzer();
    }

    // Run the target once
    switch (gFuzzTarget->run(data, size))
    {
    case stellar::FuzzResultCode::FUZZ_SUCCESS:
        return 0;
    case stellar::FuzzResultCode::FUZZ_REJECTED:
        // Input was rejected (normal for fuzzing) - do not treat as error
        return -1;
    case stellar::FuzzResultCode::FUZZ_TARGET_UNKNOWN:
        // Should not happen - we validated target at initialization
        std::fprintf(stderr, "Fuzz target became unknown at runtime\n");
        std::abort();
    default:
        std::fprintf(stderr, "Fuzz target returned unexpected result\n");
        std::abort();
    }
}