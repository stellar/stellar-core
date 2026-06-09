#pragma once

#include "util/SimpleTimer.h"
#include <atomic>
#include <chrono>
#include <medida/metrics_registry.h>
#include <util/ThreadAnnotations.h>

namespace stellar
{
// Wrapper around medida::MetricsRegistry to support registering `SimpleTimer`s
class MetricsRegistry : public medida::MetricsRegistry
{
    ANNOTATED_MUTEX(mLock);
    // Note that it is safe to hand out references to this map because values
    // have pointer stability.
    std::map<SimpleTimerName, SimpleTimer> mSimpleTimers GUARDED_BY(mLock);

    // Some SimpleTimers sit on hot paths shared by the parallel apply threads
    // (notably the bucket point-load timers), where their mutex and shared
    // counters become a cross-thread contention point. This flag lets
    // benchmarking configs turn their updates off; see
    // DISABLE_SOROBAN_METRICS_FOR_TESTING.
    bool mSimpleTimersEnabled{true};

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

    void
    setSimpleTimersEnabled(bool enabled)
    {
        mSimpleTimersEnabled = enabled;
    }

    bool
    simpleTimersEnabled() const
    {
        return mSimpleTimersEnabled;
    }
};
}
