
// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC
#include <cstdlib>
#include <cmath>
#include "Math.h"


namespace stellar
{

float rand_fraction()
{
    auto result = static_cast<float>(rand()) / static_cast<float>(RAND_MAX + 1);
    return result;
}


size_t rand_pareto(float alpha, size_t max)
{
    // from http://www.pamvotis.org/vassis/RandGen.htm
    float f = static_cast<float>(1) / static_cast<float>(pow(rand_fraction(), static_cast<float>(1) / alpha));
    auto result = static_cast<size_t>((f / 10) * max) % max;
    // LOG(INFO) << "rand_pareto " << result << " of " << max;
    return result;
}
}