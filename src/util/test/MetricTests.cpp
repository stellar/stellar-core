// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/catch.hpp"
#include "medida/histogram.h"
#include "medida/stats/sliding_window_sample.h"
#include "medida/stats/snapshot.h"
#include "util/Logging.h"
#include "util/Math.h"
#include <deque>
#include <fmt/format.h>
#include <iostream>
#include <random>
#include <sstream>

// These tests just check that medida's math is roughly sensible.
namespace
{

using uniform_dbl = std::uniform_real_distribution<double>;
using gamma_dbl = std::gamma_distribution<double>;
using uniform_u64 = std::uniform_int_distribution<uint64_t>;

// how much data to keep in memory when comparing datasets
static std::chrono::seconds const sampleCutoff(60 * 5);

// Helper for diagnostics.
void
printDistribution(std::vector<double> const& dist, size_t nbuckets = 10)
{
    // Establish bucket linear range.
    double lo = std::numeric_limits<double>::max();
    double hi = std::numeric_limits<double>::min();
    for (double d : dist)
    {
        lo = std::min(lo, d);
        hi = std::max(hi, d);
    }
    // Build a linear histogram.
    double bucketsz = (hi - lo) / nbuckets;
    std::vector<size_t> buckets(nbuckets, 0);
    size_t maxbucket = 0;
    for (double d : dist)
    {
        size_t bucket = std::min(
            nbuckets - 1, static_cast<size_t>(std::floor((d - lo) / bucketsz)));
        size_t& b = buckets.at(bucket);
        ++b;
        maxbucket = std::max(b, maxbucket);
    }
    // Print the histogram.
    size_t ncols = 40;
    size_t countPerCol = maxbucket / ncols;
    double b = bucketsz;
    LOG_INFO(DEFAULT_LOG, "{}",
             fmt::format("histogram range [{:8.2f},{:8.2f}]", lo, hi));
    ;
    for (auto const& c : buckets)
    {
        std::ostringstream oss;
        for (size_t i = 0; i < (c / countPerCol); ++i)
        {
            oss << '*';
        }
        LOG_INFO(DEFAULT_LOG, "{}",
                 fmt::format("[{:8.2f},{:8.2f}] = {:>8d} : {:s}", b,
                             (b + bucketsz), c, oss.str()));
        ;
        b += bucketsz;
    }
}

struct Percentiles
{
    Approx mP50;
    Approx mP75;
    Approx mP95;
    Approx mP98;
    Approx mP99;
    Approx mP999;
    Percentiles(double p50, double p75, double p95, double p98, double p99,
                double p999, bool flatMargin = true)
        : mP50(p50), mP75(p75), mP95(p95), mP98(p98), mP99(p99), mP999(p999)
    {
        if (flatMargin)
        {
            setFlatMargin(p999 * 0.075);
        }
        else
        {
            setSkewMargin(p999 * 0.075);
        }
    }
    void
    setFlatMargin(double m)
    {
        mP50.margin(m);
        mP75.margin(m);
        mP95.margin(m);
        mP98.margin(m);
        mP99.margin(m);
        mP999.margin(m);
    }
    void
    setSkewMargin(double m)
    {
        // When dealing with gamma distributions we've got quite a long tail, so
        // the p9x values all need more leeway.
        mP50.margin(m);
        mP75.margin(m);
        mP95.margin(m);
        mP98.margin(m * 2);
        mP99.margin(m * 4);
        mP999.margin(m * 16);
    }
    void
    checkAgainst(medida::stats::Snapshot const& snap) const
    {
        double calc50 = snap.getMedian();
        double calc75 = snap.get75thPercentile();
        double calc95 = snap.get95thPercentile();
        double calc98 = snap.get98thPercentile();
        double calc99 = snap.get99thPercentile();
        double calc999 = snap.get999thPercentile();
        CHECK(calc50 == mP50);
        CHECK(calc75 == mP75);
        CHECK(calc95 == mP95);
        CHECK(calc98 == mP98);
        CHECK(calc99 == mP99);
        CHECK(calc999 == mP999);
        if (calc50 != mP50 || calc75 != mP75 || calc95 != mP95 ||
            calc98 != mP98 || calc99 != mP99 || calc999 != mP999)
        {
            std::ostringstream oss;
            bool first = true;
            for (auto dbl : snap.getValues())
            {
                if (first)
                {
                    first = false;
                }
                else
                {
                    oss << ", ";
                }
                oss << dbl;
            }
            LOG_ERROR(DEFAULT_LOG, "failing samples: {}", oss.str());
        }
    }
};

Percentiles const constant_23_pct(23.0, 23.0, 23.0, 23.0, 23.0, 23.0);

Percentiles const uniform_1_100_pct(50.0, 75.0, 95.0, 98.0, 99.0, 99.9);

// Assuming the R interpreter can correctly calculate percentiles:
//
// $ R -q -e 'qgamma(c(0.5, 0.75, 0.95, 0.98, 0.99, 0.999), 4, scale=100)'
// > qgamma(c(0.5, 0.75, 0.95, 0.98, 0.99, 0.999), 4, scale=100)
// [1]  367.2061  510.9427  775.3657  908.4115 1004.5118 1306.2241
Percentiles const gamma_4_100_pct(367.2061, 510.9427, 775.3657, 908.4115,
                                  1004.5118, 1306.2241,
                                  /*flatMargin=*/false);

// These are private constants in the implementation of Histogram,
// but we want to reuse them here for testing SlidingWindowTester.
static const std::uint64_t kDefaultSampleSize = 1028;
static const std::chrono::seconds kDefaultWindowTime =
    std::chrono::seconds(5 * 60);

// Check that the rate-limiting of the SlidingWindowSample doesn't
// interfere with a "true" 5-minute-long (with arbitrary event count)
// sliding window.
class SlidingWindowTester
{
    medida::stats::SlidingWindowSample mSlidingWindowSample;
    medida::Clock::time_point mTimestamp;

    struct Sample
    {
        uint64_t mData;
        medida::Clock::time_point mTimeStamp;
        Sample(uint64_t d, medida::Clock::time_point t)
            : mData(d), mTimeStamp(t)
        {
        }
    };
    std::deque<Sample> mSamples;

  public:
    SlidingWindowTester()
        : mSlidingWindowSample(kDefaultSampleSize, kDefaultWindowTime)
        , mTimestamp(medida::Clock::now())
    {
        mSlidingWindowSample.Seed(stellar::gRandomEngine());
    }
    template <typename Dist, typename... Args>
    void
    addSamplesAtFrequency(std::chrono::milliseconds timeStep, Args... args)
    {
        Dist dist(std::forward<Args>(args)...);

        auto endTime = mTimestamp + sampleCutoff + sampleCutoff;
        // Add samples to the back while advancing time.
        while (mTimestamp < endTime)
        {
            uint64_t sample =
                static_cast<uint64_t>(dist(stellar::gRandomEngine));
            mSlidingWindowSample.Update(sample, mTimestamp);
            mSamples.emplace_back(sample, mTimestamp);
            mTimestamp += timeStep;
        }
        LOG_DEBUG(DEFAULT_LOG, "added samples, have {}", mSamples.size());

        // Drop values from the front that are out-of-range.
        auto dropBefore = mTimestamp - sampleCutoff;
        while (!mSamples.empty() && mSamples.front().mTimeStamp < dropBefore)
        {
            mSamples.pop_front();
        }
        LOG_DEBUG(DEFAULT_LOG, "dropped samples, now have {}", mSamples.size());
    }

    // Adds 10 minutes @ 1khz of uniform samples from [low, high]
    void
    addUniformSamplesAtHighFrequency(uint64_t low, uint64_t high)
    {
        auto freq = std::chrono::milliseconds(1);
        addSamplesAtFrequency<uniform_u64>(freq, low, high);
    }

    // Adds 10 minutes @ 30hz of uniform samples from [low, high]
    void
    addUniformSamplesAtMediumFrequency(uint64_t low, uint64_t high)
    {
        auto freq = std::chrono::milliseconds(33);
        addSamplesAtFrequency<uniform_u64>(freq, low, high);
    }

    // Adds 10 minutes @ 1hz of uniform samples from [low, high]
    void
    addUniformSamplesAtLowFrequency(uint64_t low, uint64_t high)
    {
        auto freq = std::chrono::milliseconds(1000);
        addSamplesAtFrequency<uniform_u64>(freq, low, high);
    }

    // Adds 10 minutes @ 1khz of gamma(shape,scale) samples
    void
    addGammaSamplesAtHighFrequency(double shape, double scale)
    {
        auto freq = std::chrono::milliseconds(1);
        addSamplesAtFrequency<gamma_dbl>(freq, shape, scale);
    }

    // Adds 10 minutes @ 30hz of gamma(shape,scale) samples
    void
    addGammaSamplesAtMediumFrequency(double shape, double scale)
    {
        auto freq = std::chrono::milliseconds(33);
        addSamplesAtFrequency<gamma_dbl>(freq, shape, scale);
    }

    // Adds 10 minutes @ 1hz of gamma(shape,scale) samples
    void
    addGammaSamplesAtLowFrequency(double shape, double scale)
    {
        auto freq = std::chrono::milliseconds(1000);
        addSamplesAtFrequency<gamma_dbl>(freq, shape, scale);
    }

    medida::stats::Snapshot
    getSnapshot()
    {
        return mSlidingWindowSample.MakeSnapshot();
    }

    void
    dumpData() const
    {
        for (auto s : mSamples)
        {
            std::cout << s.mTimeStamp.time_since_epoch().count() << ", "
                      << s.mData << std::endl;
        }
    }

    Percentiles
    computePercentiles(bool skewed) const
    {
        // dumpData();
        std::vector<double> data;
        data.reserve(mSamples.size());
        for (auto const& s : mSamples)
        {
            data.emplace_back(static_cast<double>(s.mData));
        }
        std::sort(data.begin(), data.end());
        medida::stats::Snapshot snp(data);
        return Percentiles(snp.getMedian(), snp.get75thPercentile(),
                           snp.get95thPercentile(), snp.get98thPercentile(),
                           snp.get99thPercentile(), snp.get999thPercentile(),
                           !skewed);
    }

    void
    checkPercentiles(bool skewed)
    {
        auto snp = computePercentiles(skewed);
        snp.checkAgainst(getSnapshot());
    }
};
}

/*****************************************************************
 * Snapshot / percentile tests
 *****************************************************************/

template <typename Dist, typename... Args>
medida::stats::Snapshot
sampleFrom(Args... args)
{
    Dist dist(std::forward<Args>(args)...);
    std::vector<double> sample;
    for (size_t i = 0; i < 10000; ++i)
    {
        sample.emplace_back(dist(stellar::gRandomEngine));
    }
    return medida::stats::Snapshot(sample);
}

TEST_CASE("percentile calculation - constant", "[percentile][medida_math]")
{
    auto snap = sampleFrom<uniform_dbl>(23.0, 23.0);
    constant_23_pct.checkAgainst(snap);
}

TEST_CASE("percentile calculation - uniform", "[percentile][medida_math]")
{
    auto snap = sampleFrom<uniform_dbl>(1.0, 100.0);
    uniform_1_100_pct.checkAgainst(snap);
}

TEST_CASE("percentile calculation - gamma", "[percentile][medida_math]")
{

    auto snap = sampleFrom<gamma_dbl>(4.0, 100.0);
    gamma_4_100_pct.checkAgainst(snap);
}

TEST_CASE("percentile calculation - R test vectors",
          "[percentile][medida_math]")
{
    // Check we're interpolating using algorithm R7 from Hyndman and Fan (1996),
    // (which is the default out of 9 available modes in R!)

    std::vector<double> t1{1.0, 2.0, 3.0, 4.0, 5.0};
    std::vector<double> t2{1.0, 2.0, 4.0, 5.0};
    std::vector<double> t3{-41.3271385943517089, -39.6477522794157267,
                           -83.1459228647872806, 63.1505921017378569,
                           180.7508987374603748};

    medida::stats::Snapshot s1(t1);
    medida::stats::Snapshot s2(t2);
    medida::stats::Snapshot s3(t3);

    CHECK(s1.getValue(0.0) == 1.0);
    CHECK(s1.getValue(0.49) == 2.96);
    CHECK(s1.getValue(0.5) == 3.0);
    CHECK(s1.getValue(0.51) == 3.04);
    CHECK(s1.getValue(0.74) == 3.96);
    CHECK(s1.getValue(0.75) == 4.0);
    CHECK(s1.getValue(0.76) == 4.04);
    CHECK(s1.getValue(0.99) == 4.96);
    CHECK(s1.getValue(1.0) == 5.0);

    CHECK(s2.getValue(0.0) == 1.0);
    CHECK(s2.getValue(0.49) == 2.94);
    CHECK(s2.getValue(0.5) == 3.0);
    CHECK(s2.getValue(0.51) == 3.06);
    CHECK(s2.getValue(0.74) == 4.22);
    CHECK(s2.getValue(0.75) == 4.25);
    CHECK(s2.getValue(0.76) == 4.28);
    CHECK(s2.getValue(0.99) == 4.97);
    CHECK(s2.getValue(1.0) == 5.0);

    CHECK(s3.getValue(0.0801686912309378386) == -69.7356940494682789);
    CHECK(s3.getValue(0.1597867833916097879) == -56.4175667691050933);
    CHECK(s3.getValue(0.2633571741171181202) == -41.2374111726776889);
    CHECK(s3.getValue(0.3473911494947969913) == -40.6729091397219236);
    CHECK(s3.getValue(0.4680381887592375278) == -39.8624571930089431);
    CHECK(s3.getValue(0.4740310576744377613) == -39.8221998248353728);
    CHECK(s3.getValue(0.5027410138864070177) == -38.5206655216221563);
    CHECK(s3.getValue(0.5345011476892977953) == -25.4611088325778212);
    CHECK(s3.getValue(0.7777949180454015732) == 76.2253556419538683);
    CHECK(s3.getValue(0.8367955621797591448) == 103.9793310095762422);
}

/*****************************************************************
 * SlidingWindowSample tests, time-based
 *****************************************************************/

TEST_CASE("sliding window percentiles - constant",
          "[slidingwindow][medida_math]")
{
    SlidingWindowTester swt;
    swt.addUniformSamplesAtHighFrequency(23, 23);
    swt.checkPercentiles(false);
}

TEST_CASE("sliding window percentiles - uniform at high frequency",
          "[slidingwindow][medida_math]")
{
    SlidingWindowTester swt;
    swt.addUniformSamplesAtHighFrequency(1, 100);
    swt.checkPercentiles(false);
}

TEST_CASE("sliding window percentiles - uniform at medium frequency",
          "[slidingwindow][medida_math]")
{
    SlidingWindowTester swt;
    swt.addUniformSamplesAtMediumFrequency(1, 100);
    swt.checkPercentiles(false);
}

TEST_CASE("sliding window percentiles - uniform at low frequency",
          "[slidingwindow][medida_math]")
{
    SlidingWindowTester swt;
    swt.addUniformSamplesAtLowFrequency(1, 100);
    swt.checkPercentiles(false);
}

TEST_CASE("sliding window percentiles - gamma at high frequency",
          "[slidingwindow][medida_math]")
{
    SlidingWindowTester swt;
    swt.addGammaSamplesAtHighFrequency(4.0, 100.0);
    swt.checkPercentiles(true);
}

TEST_CASE("sliding window percentiles - gamma at medium frequency",
          "[slidingwindow][medida_math]")
{
    SlidingWindowTester swt;
    swt.addGammaSamplesAtMediumFrequency(4.0, 100.0);
    swt.checkPercentiles(true);
}

TEST_CASE("sliding window percentiles - gamma at low frequency",
          "[slidingwindow][medida_math]")
{
    SlidingWindowTester swt;
    swt.addGammaSamplesAtLowFrequency(4.0, 100.0);
    swt.checkPercentiles(true);
}

TEST_CASE("sliding window percentiles - low frequency alternating patterns",
          "[slidingwindow][medida_math]")
{
    // Here we alternate patterns at low frequency, which should ensure
    // that we've rescaled the reservoir repeatedly between checks.
    SlidingWindowTester swt;
    swt.addGammaSamplesAtLowFrequency(4.0, 100.0);
    swt.checkPercentiles(true);

    swt.addUniformSamplesAtLowFrequency(1, 100);
    swt.checkPercentiles(false);

    swt.addGammaSamplesAtLowFrequency(4.0, 100.0);
    swt.checkPercentiles(true);

    swt.addUniformSamplesAtLowFrequency(1, 100);
    swt.checkPercentiles(false);

    swt.addGammaSamplesAtLowFrequency(4.0, 100.0);
    swt.checkPercentiles(true);
}

TEST_CASE("sliding window percentiles - alternating frequencies and patterns",
          "[slidingwindow][medida_math]")
{
    // Here we alternate patterns at low frequency, which should ensure
    // that we've rescaled the reservoir repeatedly between checks.
    SlidingWindowTester swt;
    swt.addGammaSamplesAtLowFrequency(4.0, 100.0);
    swt.checkPercentiles(true);

    swt.addUniformSamplesAtMediumFrequency(1, 100);
    swt.checkPercentiles(false);

    swt.addGammaSamplesAtHighFrequency(4.0, 100.0);
    swt.checkPercentiles(true);

    swt.addUniformSamplesAtLowFrequency(1, 100);
    swt.checkPercentiles(false);

    swt.addGammaSamplesAtMediumFrequency(4.0, 100.0);
    swt.checkPercentiles(true);

    swt.addUniformSamplesAtHighFrequency(1, 100);
    swt.checkPercentiles(false);
}

TEST_CASE("sums of nanoseconds do not overflow", "[medida_math]")
{
    // This tests that sums (and possibly other derived values of accumulated
    // nanoseconds -- billionths of a second) in medida are not stored in an
    // int64 and therefore don't overflow after a few months of accumulation at
    // decently-high frequency.
    medida::Histogram hist;
    int64_t billion = 1000000000;
    int64_t billion_billion = billion * billion;
    // the biggest int64_t is 9.2 billion billion.
    for (size_t i = 0; i < 15; ++i)
    {
        hist.Update(billion_billion);
        REQUIRE(hist.sum() >= 0);
        REQUIRE(hist.min() >= 0);
        REQUIRE(hist.max() >= 0);
        REQUIRE(hist.mean() >= 0);
        REQUIRE(hist.std_dev() >= 0);
    }
}
