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
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "herder/Herder.h"
#include "xdrpp/marshal.h"

namespace stellar
{

static std::chrono::milliseconds const MS_TO_WAIT_FOR_FETCH_REPLY{1500};
static int const MAX_REBUILD_FETCH_LIST = 1000;

template <class TrackerT>
ItemFetcher<TrackerT>::ItemFetcher(Application& app)
    : mApp(app)
    , mItemMapSize(
          app.getMetrics().NewCounter({"overlay", "memory", "item-fetch-map"}))
{
}

template <class TrackerT>
void
ItemFetcher<TrackerT>::fetch(Hash itemHash, const SCPEnvelope& envelope)
{
    CLOG(TRACE, "Overlay") << "fetch " << hexAbbrev(itemHash);
    auto entryIt = mTrackers.find(itemHash);
    if (entryIt == mTrackers.end())
    { // not being tracked
        TrackerPtr tracker = std::make_shared<TrackerT>(mApp, itemHash);
        mTrackers[itemHash] = tracker;
        mItemMapSize.inc();

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
            mItemMapSize.dec();
        }
        else
        {
            iter++;
        }
    }
}

template <class TrackerT>
void
ItemFetcher<TrackerT>::doesntHave(Hash const& itemHash, Peer::pointer peer)
{
    const auto& iter = mTrackers.find(itemHash);
    if (iter != mTrackers.end())
    {
        iter->second->doesntHave(peer);
    }
}

template <class TrackerT>
void
ItemFetcher<TrackerT>::recv(Hash itemHash)
{
    CLOG(TRACE, "Overlay") << "Recv " << hexAbbrev(itemHash);
    const auto& iter = mTrackers.find(itemHash);
    using xdr::operator==;
    if (iter != mTrackers.end())
    {
        // this code can safely be called even if recvSCPEnvelope ends up
        // calling recv on the same itemHash
        auto& waiting = iter->second->mWaitingEnvelopes;

        CLOG(TRACE, "Overlay") << "Recv " << hexAbbrev(itemHash) << " : "
                               << waiting.size();

        while (!waiting.empty())
        {
            SCPEnvelope env = waiting.back().second;
            waiting.pop_back();
            mApp.getHerder().recvSCPEnvelope(env);
        }
        // stop the timer, stop requesting the item as we have it
        iter->second->mTimer.cancel();
    }
}

Tracker::Tracker(Application& app, Hash const& hash)
    : mApp(app)
    , mNumListRebuild(0)
    , mTimer(app)
    , mItemHash(hash)
    , mTryNextPeerReset(app.getMetrics().NewMeter(
          {"overlay", "item-fetcher", "reset-fetcher"}, "item-fetcher"))
    , mTryNextPeer(app.getMetrics().NewMeter(
          {"overlay", "item-fetcher", "next-peer"}, "item-fetcher"))
{
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
        if (iter->second.statement.slotIndex < slotIndex)
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
        CLOG(TRACE, "Overlay") << "Does not have " << hexAbbrev(mItemHash);
        tryNextPeer();
    }
}

void
Tracker::tryNextPeer()
{
    // will be called by some timer or when we get a
    // response saying they don't have it
    Peer::pointer peer;

    CLOG(TRACE, "Overlay") << "tryNextPeer " << hexAbbrev(mItemHash) << " last: "
                           << (mLastAskedPeer ? mLastAskedPeer->toString()
                                              : "<none>");

    // if we don't have a list of peers to ask and we're not
    // currently asking peers, build a new list
    if (mPeersToAsk.empty() && !mLastAskedPeer)
    {
        std::set<std::shared_ptr<Peer>> peersWithEnvelope;
        for (auto const& e : mWaitingEnvelopes)
        {
            auto const& s = mApp.getOverlayManager().getPeersKnows(e.first);
            peersWithEnvelope.insert(s.begin(), s.end());
        }

        mPeersToAsk.clear();

        // move the peers that have the envelope to the back,
        // to be processed first
        for (auto const& p : mApp.getOverlayManager().getRandomPeers())
        {
            if (peersWithEnvelope.find(p) != peersWithEnvelope.end())
            {
                mPeersToAsk.emplace_back(p);
            }
            else
            {
                mPeersToAsk.emplace_front(p);
            }
        }

        mNumListRebuild++;

        CLOG(TRACE, "Overlay") << "tryNextPeer " << hexAbbrev(mItemHash)
                               << " attempt " << mNumListRebuild
                               << " reset to #" << mPeersToAsk.size();
        mTryNextPeerReset.Mark();
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
        // clear mLastAskedPeer so that we rebuild a new list
        mLastAskedPeer.reset();
        if (mNumListRebuild > MAX_REBUILD_FETCH_LIST)
        {
            nextTry = MS_TO_WAIT_FOR_FETCH_REPLY * MAX_REBUILD_FETCH_LIST;
        }
        else
        {
            nextTry = MS_TO_WAIT_FOR_FETCH_REPLY * mNumListRebuild;
        }
    }
    else
    {
        mLastAskedPeer = peer;
        CLOG(TRACE, "Overlay") << "Asking for " << hexAbbrev(mItemHash) << " to "
                               << peer->toString();
        mTryNextPeer.Mark();
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
    StellarMessage m;
    m.type(SCP_MESSAGE);
    m.envelope() = env;
    mWaitingEnvelopes.push_back(
        std::make_pair(sha256(xdr::xdr_to_opaque(m)), env));
}

void
TxSetTracker::askPeer(Peer::pointer peer)
{
    peer->sendGetTxSet(mItemHash);
}

void
QuorumSetTracker::askPeer(Peer::pointer peer)
{
    peer->sendGetQuorumSet(mItemHash);
}

template class ItemFetcher<TxSetTracker>;
template class ItemFetcher<QuorumSetTracker>;
}
