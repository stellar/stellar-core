#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/TxSetFrame.h"
#include "util/HashOfHash.h"
#include "xdr/Stellar-SCP.h"
#include "xdr/Stellar-types.h"

#include <lib/util/lrucache.hpp>

namespace stellar
{

using TxSetFramePtr = std::shared_ptr<TxSetFrame>;

// all the txsets we have learned about per ledger#
class TxSetCache
{
  public:
    TxSetCache();

    /**
     * Add @p txset identified by @p hash to local cache.
     */
    void add(Hash hash, uint64_t lastSeenSlotIndex, TxSetFramePtr txset);
    void touch(Hash hash, uint64_t lastSeenSlotIndex);

    bool contains(Hash hash) const;
    TxSetFramePtr get(Hash const& hash);

    void eraseBelow(uint64_t slotIndex);
    void eraseAt(uint64_t slotIndex);

  private:
    using TxSetFramCacheItem = std::pair<uint64, TxSetFramePtr>;
    cache::lru_cache<Hash, TxSetFramCacheItem> mTxSetCache;
};
}
