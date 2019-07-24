// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/BatchMetrics.h"

#include "medida/meter.h"
#include "medida/timer.h"

using namespace stellar;

BatchMeter::BatchMeter(medida::Meter& m, size_t batchSz)
    : mMeter(m), mBatchSz(batchSz), mCurrentCount(0)
{
}

void
BatchMeter::Mark(size_t n)
{
    mCurrentCount += n;
    if (mCurrentCount > mBatchSz)
    {
        mMeter.Mark(mCurrentCount);
        mCurrentCount = 0;
    }
}

size_t
BatchMeter::count() const
{
    return mMeter.count() + mCurrentCount;
}

BatchTimerContext::BatchTimerContext(BatchTimer& timer)
    : mTimer(timer), mStart(Clock::now())
{
}

BatchTimerContext::~BatchTimerContext()
{
    mTimer.Update(Clock::now() - mStart);
}

BatchTimer::BatchTimer(medida::Timer& timer, size_t batchSz)
    : mTimer(timer), mBatchSz(batchSz)
{
}

void
BatchTimer::Update(std::chrono::nanoseconds duration)
{
    mBatch.emplace_back(duration);
    if (mBatch.size() >= mBatchSz)
    {
        mTimer.Update(mBatch);
        mBatch.clear();
    }
}

BatchTimerContext
BatchTimer::TimeScope()
{
    return BatchTimerContext(*this);
}
