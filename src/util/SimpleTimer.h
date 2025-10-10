#pragma once

#include <medida/counter.h>
#include <medida/metrics_registry.h>

namespace stellar
{

template <typename> constexpr bool isDuration = false;

template <typename Rep, typename Period>
constexpr bool isDuration<std::chrono::duration<Rep, Period>> = true;

template <typename T> class SimpleTimerContext;

// Simple replacement for medida timer that uses an accumulator and counter,
// while keeping track of the maximums. Names are based on the constructor with
// a suffix of `sum`, `count`, or `max`. Timers can be expensive, so this class
// replaces them with a subset of the functionality using Counters internally.
template <typename Duration> class SimpleTimer
{
    static_assert(
        isDuration<Duration>,
        "SimpleTimer must be instantiated with a std::chrono::duration");
    medida::Counter& mSum;
    medida::Counter& mCount;
    // Note that we use a counter for `mMax` so it gets displayed in the
    // metrics, but this is only synced on `syncMetrics()` to avoid races.
    medida::Counter& mMaxCounter;
    std::mutex mMaxLock;
    std::int64_t mMax;

  public:
    SimpleTimer(medida::MetricsRegistry& registry, const std::string& domain,
                const std::string& type, const std::string& baseName);

    SimpleTimer(SimpleTimer<Duration>&& other);

    // Update the value of the max metric to the value of the tracked max and
    // reset the tracked max
    void syncMax();

    // Get the value of the internal `count` counter
    std::int64_t count() const;

    // Record a sample of length duration
    void Update(std::chrono::nanoseconds duration);

    SimpleTimerContext<Duration> TimeScope();
};

template <typename Duration> class SimpleTimerContext
{
    static_assert(
        isDuration<Duration>,
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
    , mMaxCounter{registry.NewCounter(
          {domain, type, std::string{base_name} + "max"})}
    , mMax{0}
{
}

template <typename Duration>
SimpleTimer<Duration>::SimpleTimer(SimpleTimer<Duration>&& other)
    : mSum{other.mSum}
    , mCount{other.mCount}
    , mMaxCounter{other.mMaxCounter}
    , mMax{other.mMax} {};

template <typename Duration>
void
SimpleTimer<Duration>::syncMax()
{

    std::lock_guard<std::mutex> lock{mMaxLock};
    mMaxCounter.set_count(mMax);
    mMax = 0;
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
    auto converted = std::chrono::duration_cast<Duration>(d).count();
    mSum.inc(converted);
    mCount.inc(1);
    {
        std::lock_guard<std::mutex> lock{mMaxLock};
        mMax = std::max(mMax, converted);
    }
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
