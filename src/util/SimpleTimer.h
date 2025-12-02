#pragma once

#include "util/ThreadAnnotations.h"
#include <medida/counter.h>
#include <medida/metric_name.h>

namespace stellar
{

class MetricsRegistry;
class SimpleTimerContext;

// We don't use a medida::MetricName for SimpleTimers to support defaulting
// of mName to "" and to allow mName to be "" (medida throws an error if the
// name is empty)
struct SimpleTimerName
{
    std::string mDomain;
    std::string mType;
    std::string mName = "";
};

bool operator<(SimpleTimerName const&, SimpleTimerName const&);

// Simple replacement for medida timer that uses an accumulator and counter
// while keeping track of the maximums. Names are based on the constructor with
// a suffix of `sum`, `count`, or `max`. Timers can be expensive, so this class
// replaces them with a subset of the functionality using Counters internally.
// In particular, medida timers own a histogram, which gives percentile
// tracking. In contrast, SimpleTimer only keeps track of the aggregate time,
// number of events, and a resettable max. For example, at the time of writing,
// SearchableBucketListSnapshotBase uses SimpleTimers for the point load
// (loading an individual entry), but medida::Timers for the bulk timers (e.g.,
// for measuring the time of a bulk lookup for all trustlines with a given
// accountID and poolID).
class SimpleTimer
{
    medida::Counter& mSum;
    medida::Counter& mSampleCount;
    // Note that we use a counter for `mMax` so it gets displayed in the
    // metrics, but it is only synced on `syncMax()` so that the value of the
    // counter is clear.
    medida::Counter& mMaxSampleValue;
    std::int64_t mMax GUARDED_BY(mLock);

    Mutex mLock;

    std::chrono::nanoseconds const mDurationUnit;

  public:
    SimpleTimer(
        MetricsRegistry& registry, SimpleTimerName const& name,
        std::chrono::nanoseconds durationUnit = std::chrono::milliseconds{1});

    // Update the value of the max metric to the value of the tracked max and
    // reset the tracked max
    void syncMax();

    // Get the value of the internal `count` counter
    std::int64_t count() const;

    // Record a sample of length duration
    void Update(std::chrono::nanoseconds duration);

    SimpleTimerContext TimeScope();
};

class SimpleTimerContext
{
    SimpleTimer& mTimer;
    bool mActive;
    std::chrono::steady_clock::time_point mStart;

  public:
    SimpleTimerContext(SimpleTimer& timer);
    ~SimpleTimerContext();
    void Reset();
    std::chrono::nanoseconds Stop();
};
}
