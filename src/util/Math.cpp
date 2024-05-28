// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "Math.h"
#include "crypto/SecretKey.h"
#include "crypto/ShortHash.h"
#include "util/GlobalChecks.h"
#include "util/UnorderedMap.h"
#include <Tracy.hpp>
#include <algorithm>
#include <autocheck/generator.hpp>
#include <catch.hpp>
#include <cmath>
#include <mutex>
#include <numeric>
#include <set>

namespace stellar
{

thread_local stellar_default_random_engine
    gRandomEngine(getLastGlobalStateSeed());
std::uniform_real_distribution<double> uniformFractionDistribution(0.0, 1.0);

double
rand_fraction()
{
    return uniformFractionDistribution(gRandomEngine);
}

bool
rand_flip()
{
    return (gRandomEngine() & 1);
}

double
closest_cluster(double p, std::set<double> const& centers)
{
    auto bestCenter = std::numeric_limits<double>::max();
    auto currDist = std::numeric_limits<double>::max();
    for (auto const& c : centers)
    {
        auto newDist = std::fabs(c - p);
        if (newDist < currDist)
        {
            bestCenter = c;
            currDist = newDist;
        }
        else
        {
            break;
        }
    }

    return bestCenter;
}

VirtualClock::duration
exponentialBackoff(uint64_t n)
{
    // Cap to 512 sec or ~8 minutes
    uint64_t const MAX_EXPONENT = 9;
    uint64_t upperBound = 1ULL << std::min(MAX_EXPONENT, n);
    uint64_t lowerBound = upperBound < 2 ? uint64_t(1) : (upperBound / 2 + 1);
    return std::chrono::seconds(rand_uniform<uint64_t>(lowerBound, upperBound));
}

std::set<double>
k_meansPP(std::vector<double> const& points, uint32_t k)
{
    if (k == 0)
    {
        throw std::runtime_error("k_means: k must be positive");
    }

    if (points.size() < k)
    {
        return std::set<double>(points.begin(), points.end());
    }

    std::set<double> centroids;

    auto backlog = points;

    auto moveIndexToCentroid = [&](size_t index) {
        releaseAssertOrThrow(index < backlog.size());
        auto it = backlog.begin() + index;
        auto val = *it;
        backlog.erase(it);
        centroids.emplace(val);
    };

    // start with a random element
    moveIndexToCentroid(rand_uniform<size_t>(0, backlog.size() - 1));

    while (centroids.size() < k && !backlog.empty())
    {
        std::vector<double> weights;
        weights.reserve(backlog.size());

        for (auto const& p : backlog)
        {
            auto closest = closest_cluster(p, centroids);
            auto d2 = closest - p;
            d2 *= d2;
            // give a non zero probability
            d2 = std::max(std::numeric_limits<double>::min(), d2);
            weights.emplace_back(d2);
        }

        // Select the next centroid based on weights, furthest away
        std::discrete_distribution<size_t> weightedDistribution(weights.begin(),
                                                                weights.end());
        auto nextIndex = weightedDistribution(gRandomEngine);
        moveIndexToCentroid(nextIndex);
    }

    return centroids;
}

std::set<double>
k_means(std::vector<double> const& points, uint32_t k)
{
    ZoneScoped;
    // initialize centroids with k-means++
    std::set<double> centroids = k_meansPP(points, k);

    // could not pick k points to start with
    if (centroids.size() < k)
    {
        return centroids;
    }

    bool recalculate = true;
    uint32_t iteration = 0;

    const uint32_t MAX_RECOMPUTE_ITERATIONS = 50;

    // Run until convergence or iteration depth exhaustion
    while (recalculate && iteration++ < MAX_RECOMPUTE_ITERATIONS)
    {
        UnorderedMap<double, std::vector<double>> assignment;
        assignment.reserve(points.size());
        recalculate = false;
        // centroid -> assigned points
        for (auto const& p : points)
        {
            // Assign each point to the closest centroid
            auto cVal = closest_cluster(p, centroids);
            assignment[cVal].push_back(p);
        }

        // Now that assignment is done, recompute centroids or converge
        std::set<double> newCentroids;
        for (auto const& a : assignment)
        {
            newCentroids.insert(
                std::accumulate(a.second.begin(), a.second.end(), 0.0) /
                a.second.size());
        }

        if (centroids != newCentroids)
        {
            recalculate = true;
            centroids = std::move(newCentroids);
        }
    }

    return centroids;
}

static std::mutex gGlobalSeedMutex;
static unsigned int lastGlobalSeed{0};

unsigned int
getLastGlobalStateSeed()
{
    std::lock_guard<std::mutex> guard(gGlobalSeedMutex);
    return lastGlobalSeed;
}

static void
reinitializeAllGlobalStateWithSeedInternal(unsigned int seed)
{
    {
        std::lock_guard<std::mutex> guard(gGlobalSeedMutex);
        lastGlobalSeed = seed;
    }
    PubKeyUtils::clearVerifySigCache();
    srand(seed);

    // gRandomEngine is a thread_local, and is initialized / initially seeded
    // with the result of calling getLastGlobalStateSeed(). This means that the
    // main thread's gRandomEngine will be initialized with the initial value of
    // lastGlobalSeed: 0. So we _reseed_ it here. But other non-main threads
    // will initialize their thread_local gRandomEngine instances as the threads
    // are launched, which should happen _after_ we've set lastGlobalSeed to
    // some nontrivial value (either test-provided, user-provded or from the
    // system PRNG), so they will pick up that nontrivial value and we don't
    // need to "reseed" them explicitly the same way.
    assert(threadIsMain());
    gRandomEngine.seed(seed);

    randHash::initialize();
}

void
initializeAllGlobalState()
{
    releaseAssert(lastGlobalSeed == 0);
    // libstdc++ and libc++ accept and use "/dev/urandom" as the token
    // identifying the system _nonblocking_ random number generator: we're not
    // after strong cryptographic randomness here, just nonblocking best-effort.
    //
    // MSVC ignores this parameter and calls into the RtlGenRandom /
    // CryptGenRandom complex (depending on Windows version) to get the seed.
    auto const seed = std::random_device("/dev/urandom")();
    reinitializeAllGlobalStateWithSeedInternal(seed);
    // shortHash needs to be initialized with a strong random seed
    shortHash::initialize();
}

#ifdef BUILD_TESTS
void
reinitializeAllGlobalStateWithSeed(unsigned int seed)
{
    reinitializeAllGlobalStateWithSeedInternal(seed);
    // shortHash seeded for tests
    shortHash::seed(seed);
    // test only prngs
    Catch::rng().seed(seed);
    autocheck::rng().seed(seed);
}

#endif
}
