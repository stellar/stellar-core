#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <map>
#include <deque>
#include <functional>
#include "xdr/Stellar-SCP.h"
#include "overlay/Peer.h"
#include "util/Timer.h"
#include "util/NonCopyable.h"
#include <util/optional.h>
#include "util/HashOfHash.h"

/*
Manages asking for Transaction or Quorum sets from Peers

The ItemFetcher returns instances of the Tracker class. There exists
exactly one tracker per item. The tracker is used both to maintain
the state of the search, as well as to isolate cancellations. Instead
of having a `stopFetching(itemID)` method, which would necessitate
extra code to keep track of the different clients, ItemFetcher stops
fetching an item when all the shared_ptrs to the item's tracker have
been released.

*/

namespace medida
{
class Counter;
}

namespace stellar
{
class TxSetFrame;
struct SCPQuorumSet;
using TxSetFramePtr = std::shared_ptr<TxSetFrame>;
using SCPQuorumSetPtr = std::shared_ptr<SCPQuorumSet>;
using AskPeer = std::function<void(Peer::pointer, Hash)>;

class Tracker
{
  private:
    AskPeer mAskPeer;

  protected:
    friend class ItemFetcher;
    Application& mApp;
    Peer::pointer mLastAskedPeer;
    int mNumListRebuild;
    std::deque<Peer::pointer> mPeersToAsk;
    VirtualTimer mTimer;
    bool mIsStopped = false;
    std::vector<std::pair<Hash, SCPEnvelope>> mWaitingEnvelopes;
    Hash mItemHash;
    medida::Meter& mTryNextPeerReset;
    medida::Meter& mTryNextPeer;

    bool clearEnvelopesBelow(uint64 slotIndex);

    void listen(const SCPEnvelope& env);

    void doesntHave(Peer::pointer peer);
    void tryNextPeer();

  public:
    explicit Tracker(Application& app, Hash const& hash, AskPeer &askPeer);
    virtual ~Tracker();

    bool hasWaitingEnvelopes() const { return mWaitingEnvelopes.size() > 0; }
};

class ItemFetcher : private NonMovableOrCopyable
{

  public:
    using TrackerPtr = std::shared_ptr<Tracker>;

    explicit ItemFetcher(Application& app, AskPeer askPeer);

    void fetch(Hash itemHash, const SCPEnvelope& envelope);

    bool isFetching(Hash itemHash) const;

    void stopFetchingBelow(uint64 slotIndex);

    void doesntHave(Hash const& itemHash, Peer::pointer peer);

    // recv: notifies all listeners of the arrival of the item
    void recv(Hash itemHash);

  protected:
    void stopFetchingBelowInternal(uint64 slotIndex);

    Application& mApp;
    std::map<Hash, std::shared_ptr<Tracker>> mTrackers;

    // NB: There are many ItemFetchers in the system at once, but we are sharing
    // a single counter for all the items being fetched by all of them. Be
    // careful, therefore, to only increment and decrement this counter, not set
    // it absolutely.
    medida::Counter& mItemMapSize;

  private:
    AskPeer mAskPeer;
};

}
