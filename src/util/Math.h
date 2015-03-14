#pragma once
#include <cstdlib>

namespace stellar
{
double rand_fraction();

size_t rand_pareto(float alpha, size_t max);
}