#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/ItemFetchQueue.h"
#include "overlay/ItemFetcher.h"
#include "util/HashOfHash.h"

#include <lib/util/lrucache.hpp>

namespace stellar
{

class Application;

using SCPQuorumSetPtr = std::shared_ptr<SCPQuorumSet>;

class ItemFetcherImpl : public ItemFetcher
{
  public:
    using AddResult = std::pair<bool, ItemKey>;

    explicit ItemFetcherImpl(Application& app);

    AddResult add(SCPQuorumSet const& qset, bool force);
    AddResult add(TransactionSet const& txSet, bool force);

    void touch(ItemKey key);
    bool contains(ItemKey key) const;

    SCPQuorumSetPtr getQuorumSet(Hash hash);
    TxSetFramePtr getTxSet(Hash hash);

    std::vector<ItemKey> fetchFor(SCPEnvelope const& envelope,
                                  Peer::pointer peer);
    void forget(ItemKey itemKey);
    void removeKnowing(Peer::pointer peer, ItemKey itemKey);

  private:
    Application& mApp;
    ItemFetchQueue mItemFetchQueue;
    cache::lru_cache<Hash, SCPQuorumSetPtr> mQSetCache;
    cache::lru_cache<Hash, TxSetFramePtr> mTxSetCache;
};
}
