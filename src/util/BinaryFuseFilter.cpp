// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/BinaryFuseFilter.h"
#include "util/siphash.h"
#include <xdrpp/marshal.h>

namespace stellar
{

template <typename T, typename U>
BinaryFuseFilter<T, U>::BinaryFuseFilter(LedgerKeySet const& keys,
                                         BinaryFuseSeed const& seed)
    : mFilter(keys.size()), mInputSeed(seed)
{
    std::vector<size_t> hashes;
    hashes.reserve(keys.size());
    for (auto const& key : keys)
    {
        SipHash24 hasher(mInputSeed.data());
        auto keybuf = xdr::xdr_to_opaque(key);
        hasher.update(keybuf.data(), keybuf.size());
        hashes.push_back(hasher.digest());
    }

    for (size_t i = 0;; ++i)
    {
        //     auto filterSeed = mInputSeed;
        //     filterSeed[0] += i;
        //     if (mFilter.populate(hashes, filterSeed))
        //     {
        //         break;
        //     }

        // TODO: Seed for SipHash24
        if (mFilter.populate(hashes, i))
        {
            break;
        }
    }
}

template <typename T, typename U>
bool
BinaryFuseFilter<T, U>::contain(LedgerKey const& key) const
{
    SipHash24 hasher(mInputSeed.data());
    auto keybuf = xdr::xdr_to_opaque(key);
    hasher.update(keybuf.data(), keybuf.size());
    return mFilter.contain(hasher.digest());
}

template class BinaryFuseFilter<uint8_t, void>;
template class BinaryFuseFilter<uint16_t, void>;
template class BinaryFuseFilter<uint32_t, void>;
}