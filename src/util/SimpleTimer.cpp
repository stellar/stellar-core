#include "SimpleTimer.h"
#include "util/MetricsRegistry.h"

namespace stellar
{

SimpleTimer::SimpleTimer(MetricsRegistry& registry,
                         medida::MetricName const& name,
                         std::chrono::nanoseconds durationUnit)
    : mSum{registry.NewCounter(
          {name.domain(), name.type(), name.name() + "sum", name.scope()})}
    , mSampleCount{registry.NewCounter(
          {name.domain(), name.type(), name.name() + "count", name.scope()})}
    , mMaxSampleValue{registry.NewCounter(
          {name.domain(), name.type(), name.name() + "max", name.scope()})}
    , mMax{0}
    , mDurationUnit(durationUnit)
{
}

void
SimpleTimer::syncMax()
{
    MutexLocker lock{mLock};
    mMaxSampleValue.set_count(mMax);
    mMax = 0;
}

std::int64_t
SimpleTimer::count() const
{
    return mSampleCount.count();
}

void
SimpleTimer::Update(std::chrono::nanoseconds d)
{
    auto converted = d / mDurationUnit;
    mSum.inc(converted);
    mSampleCount.inc(1);
    {
        MutexLocker lock{mLock};
        mMax = std::max(mMax, converted);
    }
}

SimpleTimerContext
SimpleTimer::TimeScope()
{
    return SimpleTimerContext(*this);
}

SimpleTimerContext::SimpleTimerContext(SimpleTimer& timer) : mTimer(timer)
{
    Reset();
};

SimpleTimerContext::~SimpleTimerContext()
{
    Stop();
};

void
SimpleTimerContext::Reset()
{
    mStart = std::chrono::steady_clock::now();
    mActive = true;
}

std::chrono::nanoseconds
SimpleTimerContext::Stop()
{
    if (mActive)
    {
        auto dur = std::chrono::steady_clock::now() - mStart;
        mTimer.Update(dur);
        mActive = false;
        return dur;
    }
    return std::chrono::nanoseconds{0};
}

}
