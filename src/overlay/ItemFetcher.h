#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <map>
#include <functional>
#include "generated/SCPXDR.h"
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

static std::chrono::milliseconds const MS_TO_WAIT_FOR_FETCH_REPLY{ 500 };

template<class T, class TrackerT>
class ItemFetcher : private NonMovableOrCopyable
{
public:
    class Tracker : private NonMovableOrCopyable
    {
        Application &mApp;
        ItemFetcher &mItemFetcher;
        Peer::pointer mLastAskedPeer;
        std::vector<Peer::pointer> mPeersAsked;
        VirtualTimer mTimer;
        bool mIsStopped = false;

        std::vector<SCPEnvelope> mWaitingEnvelopes;
    public:
        uint256 mItemID;
        explicit Tracker(Application &app, uint256 const& id, ItemFetcher &itemFetcher) : 
            mApp(app)
          , mItemFetcher(itemFetcher)
          , mTimer(app)
          , mItemID(id) {}

        virtual ~Tracker();

        bool clearEnvelopesBelow(uint64 slotIndex);

       
        void cancel();
        void listen(const SCPEnvelope& env);

        virtual void askPeer(Peer::pointer peer) = 0;

        void doesntHave(Peer::pointer peer);
        void itemReceived();
        void tryNextPeer();
    };
    friend Tracker;

    using TrackerPtr = std::shared_ptr<TrackerT>;

      
    explicit ItemFetcher(Application& app);
    ~ItemFetcher();
    
    void fetch(uint256 itemID, const SCPEnvelope& envelope);

    void removeTracker(const uint256& itemID);
    void stopFetchingBelow(uint64 slotIndex);

    void doesntHave(uint256 const& itemID, Peer::pointer peer);

    // recv: notifies all listeners of the arrival of the item and caches it
    void recv(uint256 itemID, T item);

protected:
    
    Application& mApp;
    std::map<uint256, std::shared_ptr<TrackerT>> mTrackers;

    // NB: There are many ItemFetchers in the system at once, but we are sharing
    // a single counter for all the items being fetched by all of them. Be
    // careful, therefore, to only increment and decrement this counter, not set
    // it absolutely.
    medida::Counter& mItemMapSize;

};

class TxSetTracker : public ItemFetcher<TxSetFramePtr, TxSetTracker>::Tracker
{
public:
    TxSetTracker(Application &app, uint256 id, ItemFetcher<TxSetFramePtr, TxSetTracker> &itemFetcher) :
        Tracker(app, id, itemFetcher) {}

    void askPeer(Peer::pointer peer) override;
};

class QuorumSetTracker : public ItemFetcher<SCPQuorumSetPtr, QuorumSetTracker>::Tracker
{
public:
    QuorumSetTracker(Application &app, uint256 id, ItemFetcher<SCPQuorumSetPtr, QuorumSetTracker> &itemFetcher) :
        Tracker(app, id, itemFetcher) {}

    void askPeer(Peer::pointer peer) override;
};


using TxSetTrackerPtr = ItemFetcher<TxSetFramePtr, TxSetTracker>::TrackerPtr;
using QuorumSetTrackerPtr = ItemFetcher<SCPQuorumSetPtr, QuorumSetTracker>::TrackerPtr;

}

