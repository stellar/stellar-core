// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "Math.h"
#include <cmath>

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
}
