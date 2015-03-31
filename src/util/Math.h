#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include <cstdlib>
#include <random>

namespace stellar
{
double rand_fraction();

size_t rand_pareto(float alpha, size_t max);

extern std::default_random_engine gRandomEngine;
}
