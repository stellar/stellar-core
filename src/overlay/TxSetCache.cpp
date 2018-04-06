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
TxSetCache::add(Hash hash, TxSetFramePtr txset)
{
    CLOG(TRACE, "Herder") << "Add TxSet " << hexAbbrev(hash);

    mTxSetCache.put(hash, txset);
}

void
TxSetCache::touch(Hash hash)
{
    if (mTxSetCache.exists(hash))
    {
        mTxSetCache.get(hash);
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
        return mTxSetCache.get(hash);
    }

    return TxSetFramePtr();
}
}
