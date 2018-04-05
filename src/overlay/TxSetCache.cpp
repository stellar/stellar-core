// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/TxSetCache.h"
#include "crypto/Hex.h"
#include "util/Logging.h"

#define TXSET_CACHE_SIZE 10000

namespace stellar
{

TxSetCache::TxSetCache() : mTxSetCache(TXSET_CACHE_SIZE)
{
}

void
TxSetCache::add(Hash hash, uint64_t lastSeenSlotIndex, TxSetFramePtr txset)
{
    CLOG(TRACE, "Herder") << "Add TxSet " << hexAbbrev(hash);

    mTxSetCache.put(hash, std::make_pair(lastSeenSlotIndex, txset));
}

void
TxSetCache::touch(Hash hash, uint64_t lastSeenSlotIndex)
{
    if (mTxSetCache.exists(hash))
    {
        auto& item = mTxSetCache.get(hash);
        item.first = std::max(item.first, lastSeenSlotIndex);
    }
}

bool
TxSetCache::contains(Hash hash) const
{
    return mTxSetCache.exists(hash);
}

TxSetFramePtr
TxSetCache::get(Hash const& hash)
{
    if (mTxSetCache.exists(hash))
    {
        return mTxSetCache.get(hash).second;
    }

    return TxSetFramePtr();
}

void
TxSetCache::eraseBelow(uint64_t slotIndex)
{
    // 0 is special mark for data that we do not know the slot index
    // it is used for state loaded from database
    mTxSetCache.erase_if([&](TxSetFramCacheItem const& i) {
        return i.first != 0 && i.first < slotIndex;
    });
}

void
TxSetCache::eraseAt(uint64_t slotIndex)
{
    mTxSetCache.erase_if(
        [&](TxSetFramCacheItem const& i) { return i.first == slotIndex; });
}
}
