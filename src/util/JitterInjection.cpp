// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#ifdef BUILD_THREAD_JITTER

#include "JitterInjection.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include <cstdlib>
#include <fmt/format.h>
#include <sstream>

namespace stellar
{

// Static member initialization
std::atomic<uint64_t> JitterInjector::sInjectionCount{0};
std::atomic<uint64_t> JitterInjector::sDelayCount{0};

static JitterInjector::Config gJitterConfig;

// Track whether current thread's RNG has been initialized
static thread_local bool gJitterRandEngineInitialized{false};

void
JitterInjector::initialize(uint32_t testSeed)
{
    releaseAssert(threadIsMain());

    // Store seed for thread-local initialization
    // Each thread will seed its own RNG on first use in injectDelay()
    gJitterRandEngineSeed = testSeed;

    // Seed the main thread's engine immediately
    gJitterRandEngine.seed(testSeed);

    resetStats();

    CLOG_DEBUG(Test, "Jitter framework initialized with seed: 0x{:08x}",
               testSeed);
}

void
JitterInjector::resetStats()
{
    sInjectionCount = 0;
    sDelayCount = 0;
}

void
JitterInjector::configure(const Config& cfg)
{
    gJitterConfig = cfg;
}

bool
JitterInjector::injectDelay(int32_t probability, uint64_t minUsec,
                            uint64_t maxUsec)
{
    // Initialize RNG on first use for this thread
    if (!gJitterRandEngineInitialized)
    {
        gJitterRandEngine.seed(gJitterRandEngineSeed);
        gJitterRandEngineInitialized = true;
    }

    // Use provided values or fall back to config defaults
    if (probability < 0)
    {
        probability = gJitterConfig.defaultProbability;
    }
    if (minUsec == 0)
    {
        minUsec = gJitterConfig.minDelayUsec;
    }
    if (maxUsec == 0)
    {
        maxUsec = gJitterConfig.maxDelayUsec;
    }

    if (maxUsec < minUsec)
    {
        throw std::invalid_argument(
            "JitterInjector::injectDelay invalid delay range");
    }
    sInjectionCount++;

    // Clamp probability to [0, 100]
    probability = std::max(0, std::min(100, probability));

    // Generate random number in [0, 100)
    int roll = rand_uniform(0, 100, gJitterRandEngine);

    if (roll >= probability)
    {
        // Probability check failed - no delay
        return false;
    }

    uint64_t delayUsec =
        rand_uniform<uint64_t>(minUsec, maxUsec, gJitterRandEngine);
    uint64_t delayMs = delayUsec / 1'000;

    CLOG_DEBUG(Test, "Jitter delay injected: {}ms (probability {}, usec {})",
               delayMs, probability, delayUsec);

    sDelayCount++;

    std::this_thread::sleep_for(std::chrono::microseconds(delayUsec));
    return true;
}

void
JitterInjector::yield()
{
    sInjectionCount++;

    std::this_thread::yield();
}

} // namespace stellar

#endif // BUILD_THREAD_JITTER
