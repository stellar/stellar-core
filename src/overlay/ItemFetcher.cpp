// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/ItemFetcher.h"
#include "main/Application.h"
#include "overlay/OverlayManager.h"
#include "util/Logging.h"
#include "medida/metrics_registry.h"
#include "herder/TxSetFrame.h"
#include "generated/StellarXDR.h"
#include <crypto/Hex.h>
#include "herder/Herder.h"

// TODO.1 I think we need to add something that after some time it retries to
// fetch qsets that it really needs.
// (https://github.com/stellar/stellar-core/issues/81)
/*

*/

namespace stellar
{

template<class T, class TrackerT>
ItemFetcher<T, TrackerT>::ItemFetcher(Application& app) :
    mApp(app)
    , mItemMapSize(
         app.getMetrics().NewCounter({ "overlay", "memory", "item-fetch-map" }))
{}

template<class T, class TrackerT>
ItemFetcher<T, TrackerT>::~ItemFetcher()
{
    mTrackers.clear();
}



template<class T, class TrackerT>
void
ItemFetcher<T, TrackerT>::fetch(uint256 itemID, const SCPEnvelope& envelope)
{
    auto entryIt = mTrackers.find(itemID);
    if(entryIt == mTrackers.end())
    { // not being tracked
        TrackerPtr tracker = std::make_shared<TrackerT>(mApp, itemID, *this);
        mTrackers[itemID] = tracker;

        tracker->listen(envelope);
        tracker->tryNextPeer();
    } else
    {
        entryIt->second->listen(envelope);
    }
}

template<class T, class TrackerT>
void
ItemFetcher<T, TrackerT>::removeTracker(const uint256& itemID)
{
    mTrackers.erase(itemID);
}

template<class T, class TrackerT>
void 
ItemFetcher<T, TrackerT>::stopFetchingBelow(uint64 slotIndex)
{
    for(auto iter = mTrackers.begin(); iter != mTrackers.end();)
    {
        if(!iter->second->clearEnvelopesBelow(slotIndex))
        {
            iter=mTrackers.erase(iter);
        } else iter++;
    }
}


template<class T, class TrackerT>
void 
ItemFetcher<T, TrackerT>::doesntHave(uint256 const& itemID, Peer::pointer peer)
{
    const auto& iter = mTrackers.find(itemID);
    if(iter != mTrackers.end())
    {
        iter->second->doesntHave(peer);
    }
}

template<class T, class TrackerT>
void 
ItemFetcher<T, TrackerT>::recv(uint256 itemID, T item)
{
    const auto& iter = mTrackers.find(itemID);

    if (iter != mTrackers.end())
    {
        iter->second->itemReceived();
    }
}






template<class T, class TrackerT>
ItemFetcher<T, TrackerT>::Tracker::~Tracker()
{
    mTimer.cancel();
    //mItemFetcher.mTrackers.erase(mItemID);
}

// returns false if no one cares about this guy anymore
template<class T, class TrackerT>
bool
ItemFetcher<T, TrackerT>::Tracker::clearEnvelopesBelow(uint64 slotIndex)
{
    for(auto iter = mWaitingEnvelopes.begin(); iter != mWaitingEnvelopes.end();)
    {
        if(iter->statement.slotIndex < slotIndex)
        {
            iter = mWaitingEnvelopes.erase(iter);
        } else iter++;
    }
    if(mWaitingEnvelopes.size()) return true;


    mPeersAsked.clear();
    mTimer.cancel();
    mLastAskedPeer = nullptr;
    mIsStopped = true;

    return false;
}





template<class T, class TrackerT>
void 
ItemFetcher<T, TrackerT>::Tracker::doesntHave(Peer::pointer peer)
{
    if (mLastAskedPeer == peer)
    {
        tryNextPeer();
    }
}

template<class T, class TrackerT>
void 
ItemFetcher<T, TrackerT>::Tracker::itemReceived()
{
    for(SCPEnvelope& env : mWaitingEnvelopes)
    {
        mApp.getHerder().recvSCPEnvelope(env);
    }
    cancel();
}

template<class T, class TrackerT>
void 
ItemFetcher<T, TrackerT>::Tracker::tryNextPeer()
{
    // will be called by some timer or when we get a 
    // response saying they don't have it
    Peer::pointer peer;

    if (mPeersAsked.size())
    {
        while (!peer && mPeersAsked.size())
        { 
            peer = mApp.getOverlayManager().getNextPeer(mPeersAsked.back());
            if (!peer)
            {
                // no longer connected to this peer.
                // try another.
                mPeersAsked.pop_back();
            }
        }
    }
    else
    {
        peer = mApp.getOverlayManager().getRandomPeer();
    }

    std::chrono::milliseconds nextTry;
    if (!peer || find(mPeersAsked.begin(), mPeersAsked.end(), peer) != mPeersAsked.end())
    {   // we have asked all our peers
        // clear list and try again in a bit
        mPeersAsked.clear();
        nextTry = MS_TO_WAIT_FOR_FETCH_REPLY * 2;
    } else
    {
        askPeer(peer);

        mLastAskedPeer = peer;
        mPeersAsked.push_back(peer);
        nextTry = MS_TO_WAIT_FOR_FETCH_REPLY;
    }

    mTimer.expires_from_now(nextTry);
    mTimer.async_wait([this]()
    {
        this->tryNextPeer();
    }, VirtualTimer::onFailureNoop);
}

template<class T, class TrackerT>
void ItemFetcher<T, TrackerT>::Tracker::cancel()
{
    mWaitingEnvelopes.clear();
    mPeersAsked.clear();
    mTimer.cancel();
    mLastAskedPeer = nullptr;
    mIsStopped = true;
    mItemFetcher.removeTracker(mItemID);
    
}

template<class T, class TrackerT>
void ItemFetcher<T, TrackerT>::Tracker::listen(const SCPEnvelope& env)
{
    mWaitingEnvelopes.push_back(env);
}


void TxSetTracker::askPeer(Peer::pointer peer)
{
    peer->sendGetTxSet(mItemID);
}

void QuorumSetTracker::askPeer(Peer::pointer peer)
{
    peer->sendGetQuorumSet(mItemID);
}


template class ItemFetcher<TxSetFramePtr, TxSetTracker>;
template class ItemFetcher<SCPQuorumSetPtr, QuorumSetTracker>;

}
