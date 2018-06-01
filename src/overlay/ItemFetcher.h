#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/TxSetFrame.h"
#include "overlay/ItemKey.h"
#include "overlay/Peer.h"

namespace stellar
{

class Application;

using SCPQuorumSetPtr = std::shared_ptr<SCPQuorumSet>;

/**
 * This class is responsible for handling envelope items - quorum and
 * transaction sets. It keeps cache of them and can requests data from network
 * when neccessary.
 */
class ItemFetcher
{
  public:
    using AddResult = std::pair<bool, ItemKey>;

    /**
     * Add quorum set to local cache. If force is false adding is only possible
     * when item was requested from remote peer. Returns pair of values - first
     * one is set to true if value was properly added to cache, second is
     * ItemKey thatn will help with identyfing that item in future.
     */
    virtual AddResult add(SCPQuorumSet const& qset, bool force) = 0;

    /**
     * Add transaction set to local cache. If force is false adding is only
     * possible when item was requested from remote peer. Returns pair of values
     * - first one is set to true if value was properly added to cache, second
     * is ItemKey thatn will help with identyfing that item in future.
     */
    virtual AddResult add(TransactionSet const& txSet, bool force) = 0;

    /**
     * Touches LRU cache for given item, ensuring that it will be available
     * longer.
     */
    virtual void touch(ItemKey key) = 0;

    /**
     * Return true if item with given key is available in cache.
     */
    virtual bool contains(ItemKey key) const = 0;

    /**
     * Return quorum set with given hash or null pointer if it is not available
     * in cache.
     */
    virtual SCPQuorumSetPtr getQuorumSet(Hash hash) = 0;

    /**
     * Return transaction set with given hash or null pointer if it is not
     * available in cache.
     */
    virtual TxSetFramePtr getTxSet(Hash hash) = 0;

    /**
     * Start fetching items for given envelope, if neccessary. Try to ask peer
     * first as it probably has those items. Returns item keys for items that
     * are not available in cache and are being fetched now.
     */
    virtual std::vector<ItemKey> fetchFor(SCPEnvelope const& envelope,
                                          Peer::pointer peer) = 0;

    /**
     * Stops fetching given item and remove it from cache.
     */
    virtual void forget(ItemKey itemKey) = 0;

    /**
     * Remove given peer from set of peers that probably have given item.
     */
    virtual void removeKnowing(Peer::pointer peer, ItemKey itemKey) = 0;

    virtual ~ItemFetcher()
    {
    }
};
}
