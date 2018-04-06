#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "item/ItemKey.h"
#include "item/ItemPeerQueue.h"
#include "overlay/FetchQueue.h"
#include "overlay/Peer.h"
#include "util/Timer.h"

#include <map>

namespace medida
{
class Counter;
class Meter;
}

namespace stellar
{

/**
 * Class responsible for fetching data items from remote peers.
 *
 * A priority queue is used with a timeout assigned to each item. Each item is
 * removed from that queue after being received and added to it immediately
 * after this object learns about it. This queue is also used to start fetch
 * again with different peer if DONT_HAVE message is received or if timeout
 * elapses.
 */
class ItemFetcher
{
  public:
    explicit ItemFetcher(Application& app);

    /**
     * Firstly it adds peer to list of knowing peers for that item, which means
     * that this peer will be asked in first order. Then it check if item is
     * already being fetched. If not, it will be added to fetch queue to be
     * immediately started.
     */
    void fetch(Peer::pointer peer, ItemKey itemKey);

    /**
     * Stop fetching given item. It can still be received if a request for it
     * was sent before calling stopFetch.
     */
    bool stopFetch(ItemKey itemKey);

    /**
     * Checks if given item is being currently fetched.
     */
    bool isFetching(ItemKey itemKey) const;

    /**
     * Called by Peer class when it receives DONT_HAVE message from given peer.
     * If that peer is also latest one which asked for given item, the fetching
     * will restart immediately with different peer.
     */
    void doesntHave(Peer::pointer peer, ItemKey itemKey);

  private:
    Application& mApp;
    VirtualTimer mFetchTimer;
    std::map<ItemKey, ItemPeerQueue> mItemPeerQueues;
    FetchQueue mFetchQueue;

    medida::Counter& mItemMapSize;
    medida::Meter& mFetchItem;
    medida::Meter& mResetPeerQueue;

    void fetchNow(ItemKey itemKey);
    std::map<ItemKey, ItemPeerQueue>::iterator firstNotEmpty();
    std::pair<Peer::pointer, std::chrono::milliseconds>
    getNextPeerAndTimeout(ItemPeerQueue& itemPeerQueue);

    void fetchFromQueue();
    std::chrono::milliseconds fetchFrom(ItemKey itemKey,
                                        ItemPeerQueue& itemPeerQueue);
    std::string getName(Peer::pointer peer) const;
};
}
