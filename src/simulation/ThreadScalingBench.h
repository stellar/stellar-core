#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

namespace stellar
{

class Config;

// Runs a thread-scaling microbenchmark in which each thread executes an
// identical, fully independent "SAC-like" workload (per-iteration small
// allocations + memcpy + sha256, on per-thread private data, with no shared
// state or locks). It sweeps thread counts (powers of two up to
// hardware_concurrency) and logs the achieved speedup/efficiency at each point.
//
// The purpose is purely diagnostic: it measures the machine's ceiling for
// scaling truly-independent threads (memory bandwidth / allocator / frequency /
// SMT), so that the parallel-apply scaling can be compared against it. If this
// microbench fails to scale at a given thread count, the parallel-apply phase
// cannot be expected to scale past that point either, and the cause is the
// machine rather than apply-path software contention.
void runThreadScalingBench(Config const& cfg);

}
