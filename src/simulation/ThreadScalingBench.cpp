// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "simulation/ThreadScalingBench.h"
#include "crypto/SHA.h"
#include "main/Config.h"
#include "util/Logging.h"
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

namespace stellar
{

namespace
{

// Total number of work iterations per data point, split evenly across the
// threads (each does kTotalIterations / numThreads, rounded down). Holding the
// total constant makes this a strong-scaling measurement: ideal scaling keeps
// wall time falling as 1/numThreads. Tunable.
constexpr uint64_t kTotalIterations = 10'000'000;


// Cheap per-thread PRNG (no shared state).
inline uint64_t
nextRand(uint64_t& s)
{
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    return s;
}

// One unit of light, "SAC-like" work: allocate a couple of small buffers, fill
// them, hash, free. Everything is thread-local; there is no shared state and no
// synchronization, so this isolates the machine's raw scaling ceiling (memory
// bandwidth, allocator, frequency, SMT) from any application-level contention.
// Returns an accumulator to defeat dead-code elimination.
uint64_t
doWork(std::vector<uint8_t>& buf, uint64_t& rng)
{
    uint64_t acc = 0;
    
    for (size_t i = 0; i < buf.size(); ++i)
    {
        buf[i] = static_cast<uint8_t>(nextRand(rng) >> 33);
    }
    auto h = sha256(buf);
    acc ^= static_cast<uint64_t>(h[0]) |
            (static_cast<uint64_t>(h[31]) << 8);
    return acc;
}

// Spawns `numThreads` independent workers that together perform kTotalIterations
// of doWork() (each thread gets an equal fraction, rounded down) in a tight loop
// with NO clock calls, and measures the wall-clock time from just before thread
// creation to after all joins. Returns the actual total iterations performed.
uint64_t
runPoint(unsigned numThreads, double& wallSecOut)
{
    uint64_t const perThread = kTotalIterations / numThreads;
    std::vector<uint64_t> sink(numThreads, 0);
    std::vector<std::thread> threads;
    threads.reserve(numThreads);

    auto t0 = std::chrono::steady_clock::now();
    for (unsigned t = 0; t < numThreads; ++t)
    {
        threads.emplace_back([&sink, t, perThread]() {
            uint64_t rng = 0x9e3779b97f4a7c15ULL + t * 0x100000001b3ULL;
            uint64_t acc = 0;
            std::vector<uint8_t> buf(256);
            for (uint64_t j = 0; j < perThread; ++j)
            {
                acc ^= doWork(buf, rng);
            }
            sink[t] = acc;
        });
    }
    for (auto& th : threads)
    {
        th.join();
    }
    auto t1 = std::chrono::steady_clock::now();
    wallSecOut = std::chrono::duration<double>(t1 - t0).count();

    // Keep the optimizer honest.
    static volatile uint64_t gSink = 0;
    uint64_t sinkAll = 0;
    for (auto v : sink)
    {
        sinkAll ^= v;
    }
    gSink ^= sinkAll;
    return perThread * numThreads;
}

} // namespace

void
runThreadScalingBench(Config const& cfg)
{
    unsigned hc = std::thread::hardware_concurrency();
    if (hc == 0)
    {
        hc = 16;
    }

    std::vector<unsigned> counts;
    for (unsigned n = 1; n <= hc; n *= 2)
    {
        counts.push_back(n);
    }
    if (counts.empty() || counts.back() != hc)
    {
        counts.push_back(hc);
    }

    CLOG_WARNING(Perf, "===== Thread-scaling microbench (independent, "
                       "non-contentious work; no shared state / no locks) =====");
    CLOG_WARNING(Perf,
                 "hardware_concurrency={}, {} total iterations split across "
                 "threads; measures the machine's raw thread-scaling ceiling",
                 hc, kTotalIterations);
    CLOG_WARNING(Perf, "{:>8s} {:>10s} {:>15s} {:>9s} {:>7s}", "threads",
                 "wall_ms", "iters/sec", "speedup", "eff%");

    double baseWall = 0.0;
    for (unsigned n : counts)
    {
        double wallSec = 0.0;
        uint64_t total = runPoint(n, wallSec);
        if (n == counts.front())
        {
            baseWall = wallSec;
        }
        double rate = wallSec > 0 ? total / wallSec : 0.0;
        // Total work is held constant, so ideal scaling is wall(N) = wall(1)/N:
        // speedup = wall(1)/wall(N) (ideal = N), efficiency = speedup / N.
        double speedup = wallSec > 0 ? baseWall / wallSec : 0.0;
        double eff = n > 0 ? (speedup / n) * 100.0 : 0.0;
        CLOG_WARNING(Perf, "{:>8d} {:>10.1f} {:>15.0f} {:>9.2f} {:>7.1f}", n,
                     wallSec * 1000.0, rate, speedup, eff);
    }
    CLOG_WARNING(Perf, "===== end thread-scaling microbench =====");
}

}
