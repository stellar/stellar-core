#pragma once

#include "util/SimpleTimer.h"
#include <chrono>
#include <medida/metrics_registry.h>
#include <util/ThreadAnnotations.h>

namespace stellar
{
// Wrapper around medida::MetricsRegistry to support registering `SimpleTimer`s
class MetricsRegistry : public medida::MetricsRegistry
{
    Mutex mLock;
    // Note that it is safe to hand out references to this map because values
    // have pointer stability.
    std::map<SimpleTimerName, SimpleTimer> mSimpleTimers GUARDED_BY(mLock);

  public:
    MetricsRegistry(std::chrono::seconds windowSize = std::chrono::seconds{30});
    SimpleTimer& NewSimpleTimer(
        SimpleTimerName const& name,
        std::chrono::nanoseconds durationUnit = std::chrono::milliseconds{1});

    // Call syncMax() on each simple timer--i.e., set max metric to the tracked
    // max, and reset max tracking.
    void syncSimpleTimerStats();
};
}
