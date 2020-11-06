// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/catch.hpp"
#include "util/Logging.h"
#include "util/Math.h"
#include <set>

using namespace stellar;

TEST_CASE("k_means", "[kmeans][!hide]")
{
    size_t const testIterations = 100;
    uint32_t const k = 3;
    uint32_t const kMeansIterations = 10;

    // Run k_means multiple times to increase probability of selecting the best
    // clustering
    auto kMeansWithIterations = [&](std::vector<double> const& points) {
        double currentBest = std::numeric_limits<double>::max();
        std::set<double> bestClusters;

        for (uint32_t i = 0; i < kMeansIterations; ++i)
        {
            double curr = 0;
            auto kMeans = k_means(points, k);
            for (auto const& p : points)
            {
                auto closest = closest_cluster(p, kMeans);
                auto d2 = closest - p;
                d2 *= d2;
                curr += d2;
            }

            if (curr < currentBest)
            {
                currentBest = curr;
                bestClusters = kMeans;
            }
        }
        return bestClusters;
    };

    auto checkClusters = [&](std::vector<double> const& points,
                             std::set<double> expectedClusters) {
        for (size_t i = 0; i < testIterations; i++)
        {
            std::set<double> res = kMeansWithIterations(points);
            CHECK(expectedClusters == res);
        }
    };

    SECTION("most points are similar")
    {
        checkClusters({1, 1, 1, 1, 1, 12, 1, 1, 1, 2, 2, 1, 13},
                      {1.0, 2.0, 12.5});
    }
    SECTION("not enough distinct points yields fewer clusters")
    {
        checkClusters({1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 1},
                      {1.0, 2.0});
    }
    SECTION("basic")
    {
        checkClusters({95, 98, 11, 71, 71, 59, 16, 67, 37, 19},
                      {20.75, 67, 96.5});
    }
    SECTION("distinct outlier")
    {
        checkClusters(
            {1010, 1010, 9, 10, 11, 24, 11999, 44, 45, 49, 58, 12000, 12001},
            {31.25, 1010.0, 12000.0});
    }
    SECTION("invalid configurations")
    {
        // k = 0
        REQUIRE_THROWS_AS(k_means({1, 2, 3}, 0), std::runtime_error);
        // k = 0 and points are empty
        REQUIRE_THROWS_AS(k_means({}, 0), std::runtime_error);
        // points are empty
        REQUIRE(k_means({}, 3).empty());
        // k = 1 produces one cluster
        REQUIRE(k_means({1, 2, 3}, 1).size() == 1);
        // k > num points returns all points as clusters
        REQUIRE(k_means({1, 2, 3}, 10).size() == 3);
    }
}
