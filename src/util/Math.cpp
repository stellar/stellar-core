// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "Math.h"
#include <cmath>

namespace stellar
{

std::default_random_engine gRandomEngine;
std::uniform_real_distribution<double> uniformFractionDistribution(0.0, 1.0);
std::bernoulli_distribution bernoulliDistribution{0.5};

double
rand_fraction()
{
    return uniformFractionDistribution(gRandomEngine);
}

size_t
rand_pareto(float alpha, size_t max)
{
    // from http://www.pamvotis.org/vassis/RandGen.htm
    float f =
        static_cast<float>(1) /
        static_cast<float>(pow(rand_fraction(), static_cast<float>(1) / alpha));
    // modified into a truncated pareto
    return static_cast<size_t>(f * max - 1) % max;
}

bool
rand_flip()
{
    return bernoulliDistribution(gRandomEngine);
}
}
