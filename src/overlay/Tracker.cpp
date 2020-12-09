// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "Tracker.h"

#include "OverlayMetrics.h"
#include "crypto/BLAKE2.h"
#include "crypto/Hex.h"
#include "herder/Herder.h"
#include "main/Application.h"
#include "medida/medida.h"
#include "overlay/OverlayManager.h"
#include "util/Logging.h"
#include "util/Math.h"
#include "util/XDROperators.h"
#include "xdrpp/marshal.h"
#include <Tracy.hpp>

namespace stellar
{

static std::chrono::milliseconds const MS_TO_WAIT_FOR_FETCH_REPLY{1500};
static int const MAX_REBUILD_FETCH_LIST = 10;

Tracker::Tracker(Application& app, Hash const& hash, AskPeer& askPeer)
    : mAskPeer(askPeer)
    , mApp(app)
    , mNumListRebuild(0)
    , mTimer(app)
    , mItemHash(hash)
    , mTryNextPeer(
          app.getOverlayManager().getOverlayMetrics().mItemFetcherNextPeer)
    , mFetchTime("fetch-" + hexAbbrev(hash), LogSlowExecution::Mode::MANUAL)
{
    assert(mAskPeer);
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
Tracker::clearEnvelopesBelow(uint64 slotIndex)
{
    ZoneScoped;
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

    return false;
}

void
Tracker::doesntHave(Peer::pointer peer)
{
    if (mLastAskedPeer == peer)
    {
        CLOG_TRACE(Overlay, "Does not have {}", hexAbbrev(mItemHash));
        tryNextPeer();
    }
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

    auto canAskPeer = [&](Peer::pointer const& p, bool peerHas) {
        auto it = mPeersAsked.find(p);
        return (p->isAuthenticated() &&
                (it == mPeersAsked.end() || (peerHas && !it->second)));
    };

    // Helper function to populate "candidates" with a set of peers, which we're
    // going to randomly select a candidate from to ask for the item.
    //
    // We want to bias the candidates set towards peers that are close to us in
    // terms of network latency, so we repeatedly lower a "nearness threshold"
    // in units of 500ms (1/3 of the MS_TO_WAIT_FOR_FETCH_REPLY) until we have a
    // "closest peers" bucket that we have at least one peer for, and keep all
    // the peers in that bucket, and then (later) randomly select from it.
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
                int64 GROUPSIZE_MS = (MS_TO_WAIT_FOR_FETCH_REPLY.count() / 3);
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

    // build the set of peers we didn't ask yet that have this envelope
    std::map<NodeID, Peer::pointer> newPeersWithEnvelope;
    for (auto const& e : mWaitingEnvelopes)
    {
        auto const& s = mApp.getOverlayManager().getPeersKnows(e.first);
        for (auto pit = s.begin(); pit != s.end(); ++pit)
        {
            auto& p = *pit;
            if (canAskPeer(p, true))
            {
                newPeersWithEnvelope.emplace(p->getPeerID(), *pit);
            }
        }
    }

    bool peerWithEnvelopeSelected = !newPeersWithEnvelope.empty();
    if (peerWithEnvelopeSelected)
    {
        procPeers(newPeersWithEnvelope, true);
    }
    else
    {
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

        nextTry = MS_TO_WAIT_FOR_FETCH_REPLY *
                  std::min(MAX_REBUILD_FETCH_LIST, mNumListRebuild);
    }
    else
    {
        mPeersAsked[mLastAskedPeer] = peerWithEnvelopeSelected;
        CLOG_TRACE(Overlay, "Asking for {} to {}", hexAbbrev(mItemHash),
                   mLastAskedPeer->toString());
        mAskPeer(mLastAskedPeer, mItemHash);
        nextTry = MS_TO_WAIT_FOR_FETCH_REPLY;
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
Tracker::listen(const SCPEnvelope& env)
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
Tracker::discard(const SCPEnvelope& env)
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
}

std::chrono::milliseconds
Tracker::getDuration()
{
    return mFetchTime.checkElapsedTime();
}
}
