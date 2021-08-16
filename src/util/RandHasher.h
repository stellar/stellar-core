// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once
#include "util/GlobalChecks.h"
#include "util/Math.h"
#include <functional>
#include <limits>

namespace stellar
{
namespace randHash
{
void initialize();
extern bool gHaveInitialized;
extern size_t gMixer;
}
template <class T, class Hasher = std::hash<T>> class RandHasher
{
  public:
    size_t
    operator()(T const& t) const
    {
        releaseAssert(randHash::gHaveInitialized);
        return Hasher()(t) ^ randHash::gMixer;
    }
};
}
