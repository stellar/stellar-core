// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once
#include "util/Math.h"
#include <functional>
#include <limits>

namespace std
{
template <class T, class Hasher = std::hash<T>> class RandHasher
{
  public:
    size_t
    operator()(T const& t) const
    {
        static size_t mixer =
            stellar::rand_uniform<size_t>(std::numeric_limits<size_t>::min(),
                                          std::numeric_limits<size_t>::max());
        return Hasher()(t) ^ mixer;
    }
};
}
