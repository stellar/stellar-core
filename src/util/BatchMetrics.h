#pragma once

// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <chrono>
#include <cstddef>
#include <vector>

namespace medida
{
class Timer;
class Meter;
class Counter;
}

namespace stellar
{

// Wrapper around medida meter type to batch updates. Use this when you are
// seeing a medida meter itself showing up in a profile: it's being updated too
// quickly. Medida happens to be threadsafe and use atomic operations for
// counters, hitting it even a few hundred times a second can start to be
// noticable.

class BatchMeter
{
    medida::Meter& mMeter;
    size_t mBatchSz;
    size_t mCurrentCount;

  public:
    BatchMeter(medida::Meter& m, size_t batchSz = 100);
    void Mark(size_t n = 1);
    size_t count() const;
};

// Same, but for medida timer type.

class BatchTimer;
class BatchTimerContext
{
    using Clock = std::chrono::high_resolution_clock;
    BatchTimer& mTimer;
    Clock::time_point mStart;

  public:
    BatchTimerContext(BatchTimer& timer);
    ~BatchTimerContext();
};

class BatchTimer
{
    medida::Timer& mTimer;
    size_t mBatchSz;
    std::vector<std::chrono::nanoseconds> mBatch;

  public:
    BatchTimer(medida::Timer& timer, size_t batchSz = 100);
    void Update(std::chrono::nanoseconds duration);
    BatchTimerContext TimeScope();
};
}
