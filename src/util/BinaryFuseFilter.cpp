// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/BinaryFuseFilter.h"
#include "util/siphash.h"
#include <algorithm>
#include <xdrpp/marshal.h>

namespace stellar
{

template <typename T, typename U>
BinaryFuseFilter<T, U>::BinaryFuseFilter(std::vector<uint64_t>& keyHashes,
                                         binary_fuse_seed_t const& seed)
    : mFilter(keyHashes.size()), mInputSeed(seed)
{
    // If too many hash collisions occur, population will fail. Retry with
    // a different seed. This is unlikely to happen once, and is statically
    // impossible to happen 10 times.
    bool populated = false;
    for (size_t i = 0; i < 10; ++i)
    {
        auto filterSeed = mInputSeed;

        // Arbitrary seed rotation if too many hash collisions occur during
        // population. Library rotates filterSeed[0], so rotate filterSeed[1]
        filterSeed[1] += i;
        if (mFilter.populate(keyHashes, filterSeed))
        {
            populated = true;
            break;
        }
    }

    // Not statically possible
    if (!populated)
    {
        throw std::runtime_error("BinaryFuseFilter failed to populate");
    }
}

template <typename T, typename U>
BinaryFuseFilter<T, U>::BinaryFuseFilter(
    SerializedBinaryFuseFilter const& xdrFilter)
    : mFilter(xdrFilter), mInputSeed([&] {
        binary_fuse_seed_t s{};
        std::copy(xdrFilter.inputHashSeed.seed.begin(),
                  xdrFilter.inputHashSeed.seed.end(), s.begin());
        return s;
    }())
{
}

template <typename T, typename U>
bool
BinaryFuseFilter<T, U>::contains(LedgerKey const& key) const
{
    SipHash24 hasher(mInputSeed.data());
    auto keybuf = xdr::xdr_to_opaque(key);
    hasher.update(keybuf.data(), keybuf.size());
    return mFilter.contain(hasher.digest());
}

template <typename T, typename U>
bool
BinaryFuseFilter<T, U>::operator==(BinaryFuseFilter<T> const& other) const
{
    return mFilter == other.mFilter && mInputSeed == other.mInputSeed;
}

template class BinaryFuseFilter<uint8_t, void>;
template class BinaryFuseFilter<uint16_t, void>;
template class BinaryFuseFilter<uint32_t, void>;
}