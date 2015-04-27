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

// TODO.1 I think we need to add something that after some time it retries to
// fetch qsets that it really needs.
// (https://github.com/stellar/stellar-core/issues/81)
/*

*/

namespace stellar
{

template<class T, class TrackerT>
ItemFetcher<T, TrackerT>::ItemFetcher(Application& app, size_t cacheSize) :
    mApp(app)
    , mCache(cacheSize)
    , mItemMapSize(
         app.getMetrics().NewCounter({ "overlay", "memory", "item-fetch-map" }))
{}

template<class T, class TrackerT>
optional<T> 
ItemFetcher<T, TrackerT>::get(uint256 itemID)
{
    if (mCache.exists(itemID))
    {
        return make_optional<T>(mCache.get(itemID));
    }
    if (auto tracker = getTracker(itemID, false))
    {
        if (tracker->isItemFound())
        {
            return make_optional<T>(tracker->get());
        }
    }
    return nullptr;
}

template<class T, class TrackerT>
typename ItemFetcher<T, TrackerT>::TrackerPtr
ItemFetcher<T, TrackerT>::getOrFetch(uint256 itemID, std::function<void(T const &item)> cb)
{
    if (auto result = get(itemID))
    {
        cb(*result);
        return nullptr;
    } else
    {
        return fetch(itemID, cb);
    }
}

template<class T, class TrackerT>
typename ItemFetcher<T, TrackerT>::TrackerPtr
ItemFetcher<T, TrackerT>::getTracker(uint256 itemID, bool create, bool *retNewTracker)
{
    TrackerPtr tracker;
    if (retNewTracker)
        *retNewTracker = false;

    auto entryIt = mTrackers.find(itemID);
    if (entryIt != mTrackers.end())
    {
        tracker = entryIt->second.lock();
    }

    if (!tracker && create)
    {
        // no entry, or the weak pointer on the tracker could not lock.
        tracker = std::make_shared<TrackerT>(mApp, itemID, *this);
        mTrackers[itemID] = tracker;

        if (retNewTracker)
            *retNewTracker = true;
    }
    return tracker;
}

template<class T, class TrackerT>
typename ItemFetcher<T, TrackerT>::TrackerPtr
ItemFetcher<T, TrackerT>::fetch(uint256 itemID, std::function<void(T const &item)> cb)
{
    bool newTracker;
    auto tracker = getTracker(itemID, true, &newTracker);

    if (tracker->isItemFound())
    {
        cb(tracker->get());
        return tracker;
    }

    if (tracker->isStopped())
    {
        // tracker was cancelled before it found the item. start a new one.
        mTrackers.erase(itemID);
        tracker = getTracker(itemID, true, &newTracker);
        mTrackers[itemID] = tracker;
    }

    tracker->listen(cb);

    if (mCache.exists(itemID))
    {
        tracker->recv(mCache.get(itemID));
    }
    else if (newTracker)
    {
        tracker->tryNextPeer();
    }

    return tracker;
}


template<class T, class TrackerT>
void 
ItemFetcher<T, TrackerT>::doesntHave(uint256 const& itemID, Peer::pointer peer)
{
    if (auto tracker = isNeeded(itemID))
    {
        tracker->doesntHave(peer);
    }
}

template<class T, class TrackerT>
void 
ItemFetcher<T, TrackerT>::recv(uint256 itemID, T const & item)
{
    if (auto tracker = isNeeded(itemID))
    {
        mCache.put(itemID, item);
        tracker->recv(item);
    }
}

template<class T, class TrackerT>
typename ItemFetcher<T, TrackerT>::TrackerPtr
ItemFetcher<T, TrackerT>::cache(Hash itemID, T const & item)
{
    mCache.put(itemID, item);
    auto result = getTracker(itemID, true);
    recv(itemID, item); // notify listeners, if any, mark as found
    return result;
}

template<class T, class TrackerT>
typename ItemFetcher<T, TrackerT>::TrackerPtr
ItemFetcher<T, TrackerT>::isNeeded(uint256 itemID)
{
    if (auto tracker = getTracker(itemID, false))
    {
        if (!tracker->isItemFound())
            return tracker;
    }
    return nullptr;
}


template<class T, class TrackerT>
ItemFetcher<T, TrackerT>::Tracker::~Tracker()
{
    cancel();
    mItemFetcher.mTrackers.erase(mItemID);
}


template<class T, class TrackerT>
bool 
ItemFetcher<T, TrackerT>::Tracker::isItemFound()
{
    return mItem != nullptr;
}

template <class T, class TrackerT>
bool ItemFetcher<T, TrackerT>::Tracker::isStopped()
{
    return mIsStopped;
}

template<class T, class TrackerT>
T ItemFetcher<T, TrackerT>::Tracker::get()
{
    return *mItem;
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
ItemFetcher<T, TrackerT>::Tracker::recv(T item)
{
    mItem = make_optional<T>(item);
    for(auto cb : mCallbacks)
    {
        cb(item);
    }
    cancel();
}

template<class T, class TrackerT>
void 
ItemFetcher<T, TrackerT>::Tracker::tryNextPeer()
{
    // will be called by some timer or when we get a 
    // response saying they don't have it

    if (!isItemFound() && mItemFetcher.isNeeded(mItemID))
    { // we still haven't found this item, and someone still wants it
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

}

template<class T, class TrackerT>
void ItemFetcher<T, TrackerT>::Tracker::cancel()
{
    mCallbacks.clear();
    mPeersAsked.clear();
    mTimer.cancel();
    mLastAskedPeer = nullptr;
    mIsStopped = true;
}

template<class T, class TrackerT>
void ItemFetcher<T, TrackerT>::Tracker::listen(std::function<void(T const &item)> cb)
{
    assert(!isItemFound());
    mCallbacks.push_back(cb);
}


void TxSetTracker::askPeer(Peer::pointer peer)
{
    peer->sendGetTxSet(mItemID);
}

void QuorumSetTracker::askPeer(Peer::pointer peer)
{
    peer->sendGetQuorumSet(mItemID);
}

void IntTracker::askPeer(Peer::pointer peer)
{
    CLOG(INFO, "Overlay") << "asked for " << hexAbbrev(mItemID);
    mAsked.push_back(peer);
}

template class ItemFetcher<int, IntTracker>;


template class ItemFetcher<TxSetFrame, TxSetTracker>;
template class ItemFetcher<SCPQuorumSet, QuorumSetTracker>;

}
