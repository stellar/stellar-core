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
    } else
    {
        return nullopt<T>();
    }

}

template<class T, class TrackerT>
typename ItemFetcher<T, TrackerT>::TrackerPtr
ItemFetcher<T, TrackerT>::getOrFetch(uint256 itemID, std::function<void(T const &item)> cb)
{
    if (mCache.exists(itemID))
    {
        cb(mCache.get(itemID));
        return nullptr;
    } else
    {
        return fetch(itemID, cb);
    }
}

template<class T, class TrackerT>
typename ItemFetcher<T, TrackerT>::TrackerPtr
ItemFetcher<T, TrackerT>::fetch(uint256 itemID, std::function<void(T const &item)> cb)
{
    auto entry = mTrackers.find(itemID);
    TrackerPtr tracker;
    bool newTracker = false;

    if (entry != mTrackers.end())
    {
        tracker = entry->second.lock();
    }
    if (!tracker)
    {
        // no entry, or the weak pointer on the tracker could not lock.
        tracker = std::make_shared<TrackerT>(mApp, itemID, *this);
        mTrackers[itemID] = tracker;
        newTracker = true;
    }

    tracker->listen(cb);
    if (mCache.exists(itemID))
    {
        mTrackers.erase(itemID);
        tracker->recv(mCache.get(itemID));
    } else if (newTracker)
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
        mTrackers.erase(itemID);
        tracker->recv(item);
    }
}

template<class T, class TrackerT>
void ItemFetcher<T, TrackerT>::cache(Hash itemID, T const & item)
{
    mCache.put(itemID, item);
    recv(itemID, item); // notify listeners, if any
}

template<class T, class TrackerT>
optional<typename ItemFetcher<T, TrackerT>::Tracker>
ItemFetcher<T, TrackerT>::isNeeded(uint256 itemID)
{
    auto entry = mTrackers.find(itemID);
    if (entry != mTrackers.end())
    {
        if (auto tracker = entry->second.lock())
        {
            return tracker;
        }
        else
        {
            mTrackers.erase(entry);
            return nullptr;
        }
    }
    return nullptr;
}


template<class T, class TrackerT>
ItemFetcher<T, TrackerT>::Tracker::~Tracker()
{
    cancel();
}


template<class T, class TrackerT>
bool 
ItemFetcher<T, TrackerT>::Tracker::isItemFound()
{
    return mItem != nullptr;
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
}

template<class T, class TrackerT>
void ItemFetcher<T, TrackerT>::Tracker::listen(std::function<void(T const &item)> cb)
{
    assert(!isItemFound());
    mCallbacks.push_back(cb);
}


void TxSetTracker::askPeer(Peer::pointer peer)
{
    CLOG(INFO, "Overlay") << " asking " << peer->getRemoteListeningPort() << " for txSet " << hexAbbrev(mItemID);
    peer->sendGetTxSet(mItemID);
}

void QuorumSetTracker::askPeer(Peer::pointer peer)
{
    CLOG(INFO, "Overlay") << " asking " << peer->getRemoteListeningPort() << " for txSet " << hexAbbrev(mItemID);
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