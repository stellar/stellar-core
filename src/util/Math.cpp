// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "Math.h"
#include "util/GlobalChecks.h"
#include <Tracy.hpp>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <set>
#include <unordered_map>

namespace stellar
{

std::default_random_engine gRandomEngine;
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

std::set<double>
k_means(std::vector<double> const& points, uint32_t k)
{
    ZoneScoped;

    if (k == 0)
    {
        throw std::runtime_error("k_means: k must be positive");
    }

    if (points.size() < k)
    {
        return std::set<double>(points.begin(), points.end());
    }

    const uint32_t MAX_RECOMPUTE_ITERATIONS = 50;

    std::set<double> centroids{rand_element(points)};
    uint32_t i = 0;

    while (centroids.size() < k && i++ < MAX_RECOMPUTE_ITERATIONS)
    {
        std::vector<double> weights;
        for (auto const& p : points)
        {
            auto closest = closest_cluster(p, centroids);
            weights.emplace_back(std::pow(std::fabs(closest - p), 2));
        }

        std::discrete_distribution<size_t> weightedDistribution(weights.begin(),
                                                                weights.end());

        // Select the next centroid based on weights
        auto nextIndex = weightedDistribution(gRandomEngine);
        releaseAssertOrThrow(nextIndex < points.size());
        centroids.insert(points[nextIndex]);
    }

    bool recalculate = true;
    uint32_t iteration = 0;

    // Run until convergence or iteration depth exhaustion
    while (recalculate && iteration++ < MAX_RECOMPUTE_ITERATIONS)
    {
        std::unordered_map<double, std::vector<double>> assignment;
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
}
