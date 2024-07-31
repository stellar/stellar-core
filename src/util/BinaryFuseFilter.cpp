// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/BinaryFuseFilter.h"
#include "util/siphash.h"
#include <algorithm>
#include <xdrpp/marshal.h>

namespace stellar
{

template <typename T>
BinaryFuseFilter<T>::BinaryFuseFilter(std::vector<uint64_t>& keyHashes,
                                      binary_fuse_seed_t const& seed)
    : mFilter(keyHashes.size(), keyHashes, seed), mHashSeed(seed)
{
}

template <typename T>
BinaryFuseFilter<T>::BinaryFuseFilter(
    SerializedBinaryFuseFilter const& xdrFilter)
    : mFilter(xdrFilter), mHashSeed([&] {
        binary_fuse_seed_t s{};
        std::copy(xdrFilter.inputHashSeed.seed.begin(),
                  xdrFilter.inputHashSeed.seed.end(), s.begin());
        return s;
    }())
{
}

template <typename T>
bool
BinaryFuseFilter<T>::contains(LedgerKey const& key) const
{
    SipHash24 hasher(mHashSeed.data());
    auto keybuf = xdr::xdr_to_opaque(key);
    hasher.update(keybuf.data(), keybuf.size());
    return mFilter.contain(hasher.digest());
}

template <typename T>
bool
BinaryFuseFilter<T>::operator==(BinaryFuseFilter<T> const& other) const
{
    return mFilter == other.mFilter && mHashSeed == other.mHashSeed;
}

template class BinaryFuseFilter<uint8_t>;
template class BinaryFuseFilter<uint16_t>;
template class BinaryFuseFilter<uint32_t>;
}