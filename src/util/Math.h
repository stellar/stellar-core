#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/util/stdrandom.h"
#include "util/Timer.h"
#include <cstdlib>
#include <random>
#include <set>
#include <stdexcept>

namespace stellar
{
double rand_fraction();

std::set<double> k_means(std::vector<double> const& points, uint32_t k);

double closest_cluster(double p, std::set<double> const& centers);

VirtualClock::duration exponentialBackoff(uint64_t n);

bool rand_flip();

typedef std::minstd_rand stellar_default_random_engine;

extern stellar_default_random_engine gRandomEngine;

template <typename T>
T
rand_uniform(T lo, T hi, stellar_default_random_engine& engine)
{
    return stellar::uniform_int_distribution<T>(lo, hi)(engine);
}

template <typename T>
T
rand_uniform(T lo, T hi)
{
    return rand_uniform<T>(lo, hi, gRandomEngine);
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

// initializes all global state that depend on prngs
// using sane default (pseudo) random values
void initializeAllGlobalState();

#ifdef BUILD_TESTS
// This function should be called any time you need to reset stellar-core's
// global state based on a seed value, such as before each fuzz run or each unit
// test. It's declared here because most cases that want to call it are already
// including Math.h in order to use gRandomEngine. It's not present in
// non-BUILD_TESTS runs because long-running single-application processes
// shouldn't be resetting globals like this mid-run -- especially not things
// like hash function keys.
void reinitializeAllGlobalStateWithSeed(unsigned int seed);
unsigned int getLastGlobalStateSeed();
#endif

}
