// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/ItemFetcher.h"
#include "main/Application.h"
#include "overlay/OverlayManager.h"
#include "util/Logging.h"
#include "medida/metrics_registry.h"
#include "herder/TxSetFrame.h"
#include "overlay/StellarXDR.h"
#include <crypto/Hex.h>
#include "herder/Herder.h"

namespace stellar
{

template <class TrackerT>
ItemFetcher<TrackerT>::ItemFetcher(Application& app)
    : mApp(app)
    , mItemMapSize(
          app.getMetrics().NewCounter({"overlay", "memory", "item-fetch-map"}))
{
}

template <class TrackerT>
void
ItemFetcher<TrackerT>::fetch(uint256 itemID, const SCPEnvelope& envelope)
{
    CLOG(TRACE, "Overlay") << "fetch " << hexAbbrev(itemID);
    auto entryIt = mTrackers.find(itemID);
    if (entryIt == mTrackers.end())
    { // not being tracked
        TrackerPtr tracker = std::make_shared<TrackerT>(mApp, itemID);
        mTrackers[itemID] = tracker;

        tracker->listen(envelope);
        tracker->tryNextPeer();
    }
    else
    {
        entryIt->second->listen(envelope);
    }
}

template <class TrackerT>
void
ItemFetcher<TrackerT>::stopFetchingBelow(uint64 slotIndex)
{
    // only perform this cleanup from the top of the stack as it causes
    // all sorts of evil side effects
    mApp.getClock().getIOService().post(
        [this, slotIndex]()
        {
            stopFetchingBelowInternal(slotIndex);
        });
}

template <class TrackerT>
void
ItemFetcher<TrackerT>::stopFetchingBelowInternal(uint64 slotIndex)
{
    for (auto iter = mTrackers.begin(); iter != mTrackers.end();)
    {
        if (!iter->second->clearEnvelopesBelow(slotIndex))
        {
            iter = mTrackers.erase(iter);
        }
        else
        {
            iter++;
        }
    }
}

template <class TrackerT>
void
ItemFetcher<TrackerT>::doesntHave(uint256 const& itemID, Peer::pointer peer)
{
    const auto& iter = mTrackers.find(itemID);
    if (iter != mTrackers.end())
    {
        iter->second->doesntHave(peer);
    }
}

template <class TrackerT>
void
ItemFetcher<TrackerT>::recv(uint256 itemID)
{
    CLOG(TRACE, "Overlay") << "Recv " << hexAbbrev(itemID);
    const auto& iter = mTrackers.find(itemID);
    using xdr::operator==;
    if (iter != mTrackers.end())
    {
        // this code can safely be called even if recvSCPEnvelope ends up
        // calling recv on the same itemID
        auto& waiting = iter->second->mWaitingEnvelopes;

        CLOG(TRACE, "Overlay") << "Recv " << hexAbbrev(itemID) << " : " << waiting.size();

        while (!waiting.empty())
        {
            SCPEnvelope env = waiting.back();
            waiting.pop_back();
            mApp.getHerder().recvSCPEnvelope(env);
        }
    }
}

Tracker::~Tracker()
{
    mTimer.cancel();
}

// returns false if no one cares about this guy anymore
bool
Tracker::clearEnvelopesBelow(uint64 slotIndex)
{
    for (auto iter = mWaitingEnvelopes.begin();
         iter != mWaitingEnvelopes.end();)
    {
        if (iter->statement.slotIndex < slotIndex)
        {
            iter = mWaitingEnvelopes.erase(iter);
        }
        else
        {
            iter++;
        }
    }
    if (!mWaitingEnvelopes.empty())
    {
        return true;
    }

    mTimer.cancel();
    mLastAskedPeer = nullptr;
    mIsStopped = true;

    return false;
}

void
Tracker::doesntHave(Peer::pointer peer)
{
    if (mLastAskedPeer == peer)
    {
        CLOG(TRACE, "Overlay") << "Does not have " << hexAbbrev(mItemID);
        tryNextPeer();
    }
}

void
Tracker::tryNextPeer()
{
    // will be called by some timer or when we get a
    // response saying they don't have it
    Peer::pointer peer;

    CLOG(TRACE, "Overlay") << "tryNextPeer " << hexAbbrev(mItemID) << " last: "
                          << (mLastAskedPeer ? mLastAskedPeer->toString()
                                             : "<none>");

    if (mPeersToAsk.empty())
    {
        mPeersToAsk = mApp.getOverlayManager().getRandomPeers();
        CLOG(TRACE, "Overlay") << "tryNextPeer " << hexAbbrev(mItemID)
                              << " reset to #" << mPeersToAsk.size();
    }

    while (!peer && !mPeersToAsk.empty())
    {
        peer = mPeersToAsk.back();
        if (!peer->isAuthenticated())
        {
            peer.reset();
        }
        mPeersToAsk.pop_back();
    }

    std::chrono::milliseconds nextTry;
    if (!peer)
    { // we have asked all our peers
        // clear list and try again in a bit
        nextTry = MS_TO_WAIT_FOR_FETCH_REPLY * 2;
    }
    else
    {
        mLastAskedPeer = peer;
        CLOG(TRACE, "Overlay") << "Asking for " << hexAbbrev(mItemID) << " to "
                              << peer->toString();
        askPeer(peer);
        nextTry = MS_TO_WAIT_FOR_FETCH_REPLY;
    }

    mTimer.expires_from_now(nextTry);
    mTimer.async_wait(
        [this]()
        {
            this->tryNextPeer();
        },
        VirtualTimer::onFailureNoop);
}

void
Tracker::listen(const SCPEnvelope& env)
{
    mWaitingEnvelopes.push_back(env);
}

void
TxSetTracker::askPeer(Peer::pointer peer)
{
    peer->sendGetTxSet(mItemID);
}

void
QuorumSetTracker::askPeer(Peer::pointer peer)
{
    peer->sendGetQuorumSet(mItemID);
}

template class ItemFetcher<TxSetTracker>;
template class ItemFetcher<QuorumSetTracker>;
}
