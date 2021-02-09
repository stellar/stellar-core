#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <cstdlib>
#include <random>
#include <set>
#include <stdexcept>

namespace stellar
{
double rand_fraction();

std::set<double> k_means(std::vector<double> const& points, uint32_t k);

double closest_cluster(double p, std::set<double> const& centers);

bool rand_flip();

typedef std::minstd_rand stellar_default_random_engine;

extern stellar_default_random_engine gRandomEngine;

template <typename T>
T
rand_uniform(T lo, T hi)
{
    return std::uniform_int_distribution<T>(lo, hi)(gRandomEngine);
}

template <typename T>
T const&
rand_element(std::vector<T> const& v)
{
    if (v.size() == 0)
    {
        throw std::range_error("rand_element on empty vector");
    }
    return v.at(rand_uniform<size_t>(0, v.size() - 1));
}

template <typename T>
T&
rand_element(std::vector<T>& v)
{
    if (v.size() == 0)
    {
        throw std::range_error("rand_element on empty vector");
    }
    return v.at(rand_uniform<size_t>(0, v.size() - 1));
}
}
