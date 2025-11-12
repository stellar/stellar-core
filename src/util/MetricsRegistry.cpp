#include "util/MetricsRegistry.h"

namespace stellar
{
MetricsRegistry::MetricsRegistry(std::chrono::seconds windowSize)
    : medida::MetricsRegistry(windowSize)
{
}

SimpleTimer&
MetricsRegistry::NewSimpleTimer(const medida::MetricName& name,
                                std::chrono::nanoseconds durationUnit)
{
    MutexLocker guard{mLock};
    return mSimpleTimers.try_emplace(name, *this, name, durationUnit)
        .first->second;
}

void
MetricsRegistry::syncMaxes()
{
    MutexLocker guard{mLock};
    for (auto& timer : mSimpleTimers)
    {
        timer.second.syncMax();
    }
}
}
