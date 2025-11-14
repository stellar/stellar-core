// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#ifdef BUILD_THREAD_JITTER

#include "JitterInjection.h"
#include "util/Logging.h"
#include <cstdlib>
#include <fmt/format.h>
#include <sstream>

namespace stellar
{

// Static member initialization
uint64_t thread_local JitterInjector::sInjectionCount{0};
uint64_t thread_local JitterInjector::sDelayCount{0};

static JitterInjector::Config gJitterConfig;

void
JitterInjector::initialize(uint32_t testSeed)
{
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
JitterInjector::injectDelay(double probability, uint32_t minNs, uint32_t maxNs)
{
    sInjectionCount++;

    // Use provided values or fall back to config defaults
    if (probability < 0.0)
    {
        probability = gJitterConfig.defaultProbability;
    }
    if (minNs == 0)
    {
        minNs = gJitterConfig.minDelayNs;
    }
    if (maxNs == 0)
    {
        maxNs = gJitterConfig.maxDelayNs;
    }

    // Clamp probability to [0.0, 1.0]
    probability = std::max(0.0, std::min(1.0, probability));

    // Generate random number in [0.0, 1.0)
    double roll = rand_uniform(0.0, 1.0, gJitterRandEngine);

    if (roll >= probability)
    {
        // Probability check failed - no delay
        return false;
    }

    // Inject delay with random duration
    if (minNs > maxNs)
    {
        std::swap(minNs, maxNs);
    }

    std::uniform_int_distribution<uint32_t> delayDist(minNs, maxNs);
    uint32_t delayNs = delayDist(gJitterRandEngine);

    uint32_t delayMs = delayNs / 1'000'000;

    CLOG_DEBUG(Test, "Jitter delay injected: {}ms (probability {:.1f}%)",
               delayMs, probability * 100.0);

    sDelayCount++;

    std::this_thread::sleep_for(std::chrono::nanoseconds(delayNs));
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
