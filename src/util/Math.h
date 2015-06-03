#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <cstdlib>
#include <random>

namespace stellar
{
double rand_fraction();

size_t rand_pareto(float alpha, size_t max);

bool rand_flip();

extern std::default_random_engine gRandomEngine;
}
