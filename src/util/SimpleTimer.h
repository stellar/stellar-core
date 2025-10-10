#pragma once

#include <medida/counter.h>
#include <medida/metrics_registry.h>

namespace stellar
{

template <typename> struct IsDuration : std::false_type
{
};

template <typename Rep, typename Period>
struct IsDuration<std::chrono::duration<Rep, Period>> : std::true_type
{
};

template <typename T> class SimpleTimerContext;

// Simple replacement for medida timer that uses an accumulator and counter,
// while keeping track of the maximums. Names are based on the constructor with
// a suffix of `sum`, `count`, or `max`. Timers can be expensive, so this class
// replaces them with a subset of the functionality using Counters internally.
template <typename Duration> class SimpleTimer
{
    static_assert(
        IsDuration<Duration>::value,
        "SimpleTimer must be instantiated with a std::chrono::duration");
    medida::Counter& mSum;
    medida::Counter& mCount;
    // Note that we use a counter for `mMax` so it gets displayed in the
    // metrics.
    medida::Counter& mMax;

  public:
    SimpleTimer(medida::MetricsRegistry& registry, const std::string& domain,
                const std::string& type, const std::string& baseName);

    // Reset the max counter
    void clearMax();

    // Reset all internal counters
    void clear();

    // Get the value of the internal `count` counter
    std::int64_t count() const;

    // Record a sample of length duration
    void Update(std::chrono::nanoseconds duration);

    SimpleTimerContext<Duration> TimeScope();
};

template <typename Duration> class SimpleTimerContext
{
    static_assert(
        IsDuration<Duration>(),
        "SimpleTimerContext must be instantiated with a std::chrono::duration");
    SimpleTimer<Duration>& mTimer;
    bool mActive;
    std::chrono::steady_clock::time_point mStart;

  public:
    SimpleTimerContext(SimpleTimer<Duration>& timer);
    ~SimpleTimerContext();
    void Reset();
    std::chrono::nanoseconds Stop();
};

template <typename Duration>
SimpleTimer<Duration>::SimpleTimer(medida::MetricsRegistry& registry,
                                   const std::string& domain,
                                   const std::string& type,
                                   const std::string& base_name)
    : mSum{registry.NewCounter({domain, type, std::string{base_name} + "sum"})}
    , mCount{registry.NewCounter(
          {domain, type, std::string{base_name} + "count"})}
    , mMax{registry.NewCounter({domain, type, std::string{base_name} + "max"})}
{
}

template <typename Duration>
void
SimpleTimer<Duration>::clearMax()
{
    mMax.clear();
}

template <typename Duration>
void
SimpleTimer<Duration>::clear()
{
    mSum.clear();
    mCount.clear();
}

template <typename Duration>
std::int64_t
SimpleTimer<Duration>::count() const
{
    return mCount.count();
}

template <typename Duration>
void
SimpleTimer<Duration>::Update(std::chrono::nanoseconds d)
{
    auto converted = std::chrono::duration_cast<Duration>(d);
    mSum.inc(converted.count());
    mCount.inc(1);
    mMax.set_count(std::max(mMax.count(), converted.count()));
}

template <typename Duration>
SimpleTimerContext<Duration>
SimpleTimer<Duration>::TimeScope()
{
    return SimpleTimerContext<Duration>(*this);
}

template <typename Duration>
SimpleTimerContext<Duration>::SimpleTimerContext(SimpleTimer<Duration>& timer)
    : mTimer(timer)
{
    Reset();
};

template <typename Duration> SimpleTimerContext<Duration>::~SimpleTimerContext()
{
    Stop();
};

template <typename Duration>
void
SimpleTimerContext<Duration>::Reset()
{
    mStart = std::chrono::steady_clock::now();
    mActive = true;
}

template <typename Duration>
std::chrono::nanoseconds
SimpleTimerContext<Duration>::Stop()
{
    if (mActive)
    {
        auto dur = std::chrono::steady_clock::now() - mStart;
        mTimer.Update(dur);
        mActive = false;
        return dur;
    }
    return Duration{0};
}

};
