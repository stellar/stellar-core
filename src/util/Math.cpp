// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "Math.h"
#include <cstdlib>
#include <cmath>
#include <random>

namespace stellar
{

std::default_random_engine generator;
std::uniform_real_distribution<double> uniformFractionDistribution(0.0,1.0);

double rand_fraction()
{
    return uniformFractionDistribution(generator);
}


size_t rand_pareto(float alpha, size_t max)
{
    // from http://www.pamvotis.org/vassis/RandGen.htm
    float f = static_cast<float>(1) / static_cast<float>(pow(rand_fraction(), static_cast<float>(1) / alpha));
    // modified into a truncated pareto
    return static_cast<size_t>(f * max - 1) % max;
}
}
