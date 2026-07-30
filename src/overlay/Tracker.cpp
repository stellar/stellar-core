// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "Tracker.h"

#include "OverlayMetrics.h"
#include "crypto/BLAKE2.h"
#include "crypto/Hex.h"
#include "herder/Herder.h"
#include "main/Application.h"
#include "medida/meter.h"
#include "overlay/OverlayManager.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/Math.h"
#include <Tracy.hpp>

namespace stellar
{

std::chrono::milliseconds const Tracker::MS_TO_WAIT_FOR_FETCH_PROGRESS{1500};
int const Tracker::MAX_REBUILD_FETCH_LIST = 10;

Tracker::Tracker(Application& app, Hash const& hash, AskPeer& askPeer,
                 ItemFetcherKind kind)
    : mAskPeer(askPeer)
    , mApp(app)
    , mNumListRebuild(0)
    , mTimer(app)
    , mItemHash(hash)
    , mKind(kind)
    , mTryNextPeer(
          app.getOverlayManager().getOverlayMetrics().mItemFetcherNextPeer)
    , mFetchTime("fetch-" + hexAbbrev(hash), LogSlowExecution::Mode::MANUAL)
{
    releaseAssert(mAskPeer);

    if (mKind == ItemFetcherKind::TxSet &&
        mApp.getHerder().protocolAllowsEmptyTxSetValues())
    {
        mGraceEnabled = true;
        mGraceStart = mApp.getClock().now();
        mGraceDeadline = mGraceStart + MS_TO_WAIT_FOR_FETCH_PROGRESS;
    }
}

Tracker::~Tracker()
{
    cancel();
}

SCPEnvelope
Tracker::pop()
{
    auto env = mWaitingEnvelopes.back().second;
    mWaitingEnvelopes.pop_back();
    return env;
}

// returns false if no one cares about this guy anymore
bool
Tracker::clearEnvelopesOutsideRange(std::optional<uint64> minSlot,
                                    std::optional<uint64> maxSlot,
                                    uint64 slotToKeep)
{
    ZoneScoped;
    for (auto iter = mWaitingEnvelopes.begin();
         iter != mWaitingEnvelopes.end();)
    {
        auto const index = iter->second.statement.slotIndex;
        bool const outsideRange =
            (minSlot && index < *minSlot) || (maxSlot && index > *maxSlot);
        if (outsideRange && index != slotToKeep)
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

    return false;
}

void
Tracker::doesntHave(Peer::pointer peer)
{
    if (mLastAskedPeer == peer)
    {
        CLOG_TRACE(Overlay, "Does not have {}", hexAbbrev(mItemHash));
        // Any claim this peer made was wrong.
        mClaimingPeers.erase(peer);
        tryNextPeer();
    }
}

void
Tracker::peerClaims(Peer::pointer peer)
{
    ZoneScoped;
    mClaimingPeers.insert(peer);
    if (!mLastAskedPeer)
    {
        // No ask is outstanding. Act on the claim immediately
        mTimer.cancel();
        tryNextPeer();
    }
}

void
Tracker::seedClaim(Peer::pointer peer)
{
    mClaimingPeers.insert(peer);
}

void
Tracker::tryNextPeer()
{
    ZoneScoped;
    // will be called by some timer or when we get a
    // response saying they don't have it
    CLOG_TRACE(Overlay, "tryNextPeer {} last: {}", hexAbbrev(mItemHash),
               (mLastAskedPeer ? mLastAskedPeer->toString() : "<none>"));

    if (mLastAskedPeer)
    {
        mTryNextPeer.Mark();
        mLastAskedPeer.reset();
    }

    // canAskPeer is best effort and send happens asynchronously; in the worst
    // case, we'll place something in the queue that will subsequently be
    // discarded due to a peer drop.
    auto canAskPeer = [&](Peer::pointer const& p, bool peerHas) {
        auto it = mPeersAsked.find(p);
        return (p->isAuthenticatedAtomic() &&
                (it == mPeersAsked.end() || (peerHas && !it->second)));
    };

    // Helper function to populate "candidates" with a set of peers, which we're
    // going to randomly select a candidate from to ask for the item.
    //
    // We want to bias the candidates set towards peers that are close to us in
    // terms of network latency, so we repeatedly lower a "nearness threshold"
    // in units of 500ms (1/3 of the MS_TO_WAIT_FOR_FETCH_PROGRESS) until we
    // have a "closest peers" bucket that we have at least one peer for, and
    // keep all the peers in that bucket, and then (later) randomly select from
    // it.
    //
    // if the map of peers passed in is for peers that claim to have the data we
    // need, `peersHave` is also set to true. in this case, the candidate list
    // will also be populated with peers that we asked before but that since
    // then received the data that we need
    std::vector<Peer::pointer> candidates;
    int64 curBest = INT64_MAX;

    auto procPeers = [&](std::map<NodeID, Peer::pointer> const& peerMap,
                         bool peersHave) {
        for (auto& mp : peerMap)
        {
            auto& p = mp.second;
            if (canAskPeer(p, peersHave))
            {
                int64 GROUPSIZE_MS =
                    (MS_TO_WAIT_FOR_FETCH_PROGRESS.count() / 3);
                int64 plat = p->getPing().count() / GROUPSIZE_MS;
                if (plat < curBest)
                {
                    candidates.clear();
                    curBest = plat;
                    candidates.emplace_back(p);
                }
                else if (curBest == plat)
                {
                    candidates.emplace_back(p);
                }
            }
        }
    };

    // Whether a peer that relayed an SCP envelope can be assumed to possess
    // the item it references. For quorum sets, always: an envelope is never
    // relayed without its qset.  For tx sets, this function returns true prior
    // to the protocol that allows empty-tx-set values, as well as for peers
    // running pre-HAVE_TX_SET overlay versions
    auto relayerImpliesHave = [&](Peer::pointer const& p) {
        return mKind == ItemFetcherKind::QuorumSet ||
               !mApp.getHerder().protocolAllowsEmptyTxSetValues() ||
               p->getRemoteOverlayVersion() <
                   Peer::FIRST_OVERLAY_VERSION_SUPPORTING_HAVE_TX_SET;
    };

    std::map<NodeID, Peer::pointer> newPeersWithEnvelope;
    for (auto const& e : mWaitingEnvelopes)
    {
        auto const& s = mApp.getOverlayManager().getPeersKnows(e.first);
        for (auto pit = s.begin(); pit != s.end(); ++pit)
        {
            auto& p = *pit;
            if (relayerImpliesHave(p))
            {
                mClaimingPeers.insert(p);
            }
            else if (canAskPeer(p, false))
            {
                newPeersWithEnvelope.emplace(p->getPeerID(), *pit);
            }
        }
    }

    // Ask-able peers believed to hold the item
    std::map<NodeID, Peer::pointer> claimingPeers;
    for (auto const& p : mClaimingPeers)
    {
        if (canAskPeer(p, true))
        {
            claimingPeers.emplace(p->getPeerID(), p);
        }
    }

    // Claim grace period: with no peer known to hold the tx set, prefer waiting
    // a bounded time for a HAVE_TX_SET claim over blind-asking a peer that
    // likely does not have it yet. An arriving claim preempts the wait.  If the
    // grace period has expired we fall through to the relayer/random tiers as
    // usual.
    auto const now = mApp.getClock().now();
    if (claimingPeers.empty() && mGraceEnabled && now < mGraceDeadline)
    {
        auto const wait = std::chrono::duration_cast<std::chrono::milliseconds>(
            mGraceDeadline - now);
        mTimer.expires_from_now(wait);
        mTimer.async_wait([this]() { this->tryNextPeer(); },
                          VirtualTimer::onFailureNoop);
        return;
    }

    bool claimTierSelected = false;
    bool selectedPeersHave = false;
    if (!claimingPeers.empty())
    {
        // Prefer peers who claim to have the item being fetched
        claimTierSelected = true;
        selectedPeersHave = true;
        procPeers(claimingPeers, true);
    }
    else if (!newPeersWithEnvelope.empty())
    {
        // If no peers claim to have the item, fall back on those who claim to
        // have at least heard of it via SCP messages
        procPeers(newPeersWithEnvelope, false);
    }
    else
    {
        // If all else fails, ask a random peer
        auto& inPeers = mApp.getOverlayManager().getInboundAuthenticatedPeers();
        auto& outPeers =
            mApp.getOverlayManager().getOutboundAuthenticatedPeers();
        procPeers(inPeers, false);
        procPeers(outPeers, false);
    }

    // pick a random element from the candidate list
    if (!candidates.empty())
    {
        mLastAskedPeer = rand_element(candidates);
    }

    std::chrono::milliseconds nextTry;
    if (!mLastAskedPeer)
    {
        // we have asked all our peers, reset the list and try again after a
        // pause
        mNumListRebuild++;
        mPeersAsked.clear();

        CLOG_TRACE(Overlay, "tryNextPeer {} restarting fetch #{}",
                   hexAbbrev(mItemHash), mNumListRebuild);

        nextTry = MS_TO_WAIT_FOR_FETCH_PROGRESS *
                  std::min(MAX_REBUILD_FETCH_LIST, mNumListRebuild);
    }
    else
    {
        auto& om = mApp.getOverlayManager().getOverlayMetrics();
        if (claimTierSelected)
        {
            // Record that we requested from a peer who claims to have the data
            // we want
            om.mItemFetcherClaimAsk.Mark();
        }
        // Record the grace period outcome once, at the first ask: did waiting
        // for a claim land us on a claimer, or did we fall back to a blind
        // ask? mGraceEnabled is only set for TxSet fetches.
        if (mGraceEnabled && !mGraceResolved)
        {
            mGraceResolved = true;
            om.mItemFetcherClaimGraceWait.Update(now - mGraceStart);
            if (claimTierSelected)
            {
                om.mItemFetcherClaimGraceSatisfied.Mark();
            }
            else
            {
                om.mItemFetcherClaimGraceExpired.Mark();
            }
        }
        mPeersAsked[mLastAskedPeer] = selectedPeersHave;
        CLOG_TRACE(Overlay, "Asking for {} to {}", hexAbbrev(mItemHash),
                   mLastAskedPeer->toString());
        mAskPeer(mLastAskedPeer, mItemHash);
        nextTry = MS_TO_WAIT_FOR_FETCH_PROGRESS;
    }

    mTimer.expires_from_now(nextTry);
    mTimer.async_wait([this]() { this->tryNextPeer(); },
                      VirtualTimer::onFailureNoop);
}

static std::function<bool(std::pair<Hash, SCPEnvelope> const&)>
matchEnvelope(SCPEnvelope const& env)
{
    return [&env](std::pair<Hash, SCPEnvelope> const& x) {
        return x.second == env;
    };
}

void
Tracker::listen(SCPEnvelope const& env)
{
    ZoneScoped;
    mLastSeenSlotIndex = std::max(env.statement.slotIndex, mLastSeenSlotIndex);

    // don't track the same envelope twice
    auto matcher = matchEnvelope(env);
    auto it = std::find_if(mWaitingEnvelopes.begin(), mWaitingEnvelopes.end(),
                           matcher);
    if (it != mWaitingEnvelopes.end())
    {
        return;
    }

    StellarMessage m;
    m.type(SCP_MESSAGE);
    m.envelope() = env;

    // NB: hash here is BLAKE2 of StellarMessage because that is
    // what the floodmap is keyed by, and we're storing its keys
    // in mWaitingEnvelopes, not the mItemHash that is the SHA256
    // of the item being tracked.
    mWaitingEnvelopes.push_back(std::make_pair(xdrBlake2(m), env));
}

void
Tracker::discard(SCPEnvelope const& env)
{
    ZoneScoped;
    auto matcher = matchEnvelope(env);
    mWaitingEnvelopes.erase(std::remove_if(std::begin(mWaitingEnvelopes),
                                           std::end(mWaitingEnvelopes),
                                           matcher),
                            std::end(mWaitingEnvelopes));
}

void
Tracker::cancel()
{
    mTimer.cancel();
    mLastSeenSlotIndex = 0;
    mClaimingPeers.clear();
}

std::chrono::milliseconds
Tracker::getDuration()
{
    return mFetchTime.checkElapsedTime();
}
}
