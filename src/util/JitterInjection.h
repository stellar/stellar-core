#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

/**
 * Jitter Injection Framework for Finding Concurrency Bugs
 *
 * This framework implements probabilistic thread-safe delay injection to
 * discover threading bugs that are sensitive to specific interleavings. By
 * introducing random delays at strategic points, we can force different thread
 * scheduling patterns and expose race conditions, deadlocks, and ordering
 * violations.
 *
 * Design:
 * - Uses the same RNG seed as stellar-core's testing infrastructure
 * - Probabilistic: delays only happen with a configured probability
 * - Easy integration: simple macros that compile away in production
 * - Reproducible: same seed = same failure (great for debugging)
 *
 * Usage:
 *
 * 1. Basic delay injection (must be called from main thread or in test
 * context): JITTER_INJECT_DELAY();  // 10% chance of small delay
 *
 * 2. Custom probability:
 *    JITTER_INJECT_DELAY_PROBABILITY(0.5);  // 50% chance
 *
 * 3. Custom delay range (in milliseconds):
 *    JITTER_INJECT_DELAY_CUSTOM(0.5, 10, 100);  // 50% chance, 10-100ms
 *
 * 4. Force context switch without delay:
 *    JITTER_YIELD();  // Always yields, no delay
 *
 * 5. At synchronization points:
 *    {
 *        std::lock_guard<std::mutex> lock(myMutex);
 *        JITTER_INJECT_DELAY();  // After lock acquired
 *        // critical section
 *    }
 *    JITTER_INJECT_DELAY();  // After lock released
 *
 * 6. Around condition variable operations:
 *    myCondVar.wait(lock);
 *    JITTER_INJECT_DELAY();  // After waking up
 */

#include "util/Math.h"
#include <chrono>
#include <mutex>
#include <optional>
#include <random>
#include <thread>

#ifdef BUILD_THREAD_JITTER

namespace stellar
{

static thread_local stellar_default_random_engine gJitterRandEngine;
static uint32_t gJitterRandEngineSeed{0};

class JitterInjector
{
  private:
    // Use the same RNG engine type as stellar-core
    // Seeded from the test's global RNG seed for reproducibility
    static std::atomic<uint64_t> sInjectionCount;
    static std::atomic<uint64_t> sDelayCount;

  public:
    struct Config
    {
        // Default probability % (0 - 100) for delay injection
        // Default 10% chance of delay at each injection point
        int32_t defaultProbability{10};

        // Default delay range in nanoseconds
        uint32_t minDelayNs{100000};
        uint32_t maxDelayNs{10000000}; // 100 microseconds - 10 milliseconds
    };

    // Initialize the jitter framework with current test seed
    // Called automatically at test startup via
    // reinitializeAllGlobalStateWithSeed
    static void initialize(uint32_t testSeed);

    // Reset statistics
    static void resetStats();

    // Get statistics
    static uint64_t getInjectionCount()
    {
        return sInjectionCount;
    }
    static uint64_t getDelayCount()
    {
        return sDelayCount;
    }

    // Configure jitter behavior
    static void configure(const Config& cfg);

    // Main injection point: probabilistically delay with random duration
    // Returns true if a delay was injected, false otherwise
    // delay range is in nanoseconds
    static bool injectDelay(int32_t probability = -1, uint32_t minNs = 0,
                            uint32_t maxNs = 0);

    // Force a context switch without additional delay
    static void yield();
};

} // namespace stellar

// Macros for easy integration in code
// These compile away to nothing in non-test builds

/**
 * Basic jitter injection with default probability (10%)
 * Use at critical sections: after mutex acquisition, before/after
 * condition variable operations, etc.
 */
#define JITTER_INJECT_DELAY() stellar::JitterInjector::injectDelay()

/**
 * Jitter injection with custom probability (0 - 100)
 * Example: JITTER_INJECT_DELAY_PROBABILITY(0.25)  // 25% chance
 */
#define JITTER_INJECT_DELAY_PROBABILITY(prob) \
    stellar::JitterInjector::injectDelay(prob)

/**
 * Jitter injection with full customization
 * Usage: JITTER_INJECT_DELAY_CUSTOM(probability, min_ns, max_ns)
 * Example: JITTER_INJECT_DELAY_CUSTOM(0.5, 5000000, 50000000)
 */
#define JITTER_INJECT_DELAY_CUSTOM(prob, minNs, maxNs) \
    stellar::JitterInjector::injectDelay(prob, minNs, maxNs)

/**
 * Force a context switch without delay
 * Use when you want to test interleaving without random delays
 */
#define JITTER_YIELD() stellar::JitterInjector::yield()

#else

// Non-jitter builds: compile away to nothing
#define JITTER_INJECT_DELAY()
#define JITTER_INJECT_DELAY_PROBABILITY(prob)
#define JITTER_INJECT_DELAY_CUSTOM(prob, minNs, maxNs)
#define JITTER_YIELD()

#endif // BUILD_THREAD_JITTER
