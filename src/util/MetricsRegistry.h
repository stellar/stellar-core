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
    // Note that we can use SimpleTimer instead of shared_ptr as medida does
    // because we don't need runtime polymorphism nor do we have a GetAllMetrics
    // call that will return shared_ptrs
    std::map<medida::MetricName, SimpleTimer> mSimpleTimers GUARDED_BY(mLock);

  public:
    MetricsRegistry(std::chrono::seconds windowSize = std::chrono::seconds(30));
    SimpleTimer& NewSimpleTimer(medida::MetricName const& name,
                                std::chrono::nanoseconds durationUnit);

    // Call syncMax() on each simple timer--i.e., set max metric to the tracked
    // max, and reset max tracking.
    void syncMaxes();
};
}
