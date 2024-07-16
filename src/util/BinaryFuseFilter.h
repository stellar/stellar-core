#pragma once

// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/binaryfusefilter.h"
#include "util/NonCopyable.h"

namespace stellar
{

namespace
{
template <typename T,
          class = typename std::enable_if_t<std::is_unsigned<T>::value>>
class BinaryFuseFilter : public NonMovableOrCopyable
{
  private:
    binary_fuse_t<T> mFilter;

  public:
    BinaryFuseFilter(size_t size) : mFilter(size)
    {
    }

    [[nodiscard]] bool
    populate(std::vector<size_t>& hashes, size_t rngSeed)
    {
        return mFilter.populate(hashes, rngSeed);
    }

    bool
    contain(size_t hash)
    {
        return mFilter.contain(hash);
    }
};
}

// False positive rate: 1/256
// Approximate bits per entry: 9
typedef BinaryFuseFilter<uint8_t> BinaryFuseFilter8;

// False positive rate: 1/65536
// Approximate bits per entry: 18
typedef BinaryFuseFilter<uint16_t> BinaryFuseFilter16;

// False positive rate: 1 / 4 billion
// Approximate bits per entry: 36
typedef BinaryFuseFilter<uint32_t> BinaryFuseFilter32;
}