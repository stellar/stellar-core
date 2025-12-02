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
    // max, and reset max tracking. That is, consistent periods are created by
    // calling this method at regular intervals. In the normal workflow, we
    // expect this to be called when the `/metrics` endpoint is hit
    // (equivalently when `ApplicationImpl::syncAllMetrics` is called). This
    // should happen regularly: at the time of writing, this is done by the core
    // prometheus exporter.
    void syncSimpleTimerStats();
};
}
