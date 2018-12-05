// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "Tracker.h"

#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "herder/Herder.h"
#include "main/Application.h"
#include "medida/medida.h"
#include "overlay/OverlayManager.h"
#include "util/Logging.h"
#include "util/XDROperators.h"
#include "xdrpp/marshal.h"

namespace stellar
{

static std::chrono::milliseconds const MS_TO_WAIT_FOR_FETCH_REPLY{1500};
static int const MAX_REBUILD_FETCH_LIST = 1000;

Tracker::Tracker(Application& app, Hash const& hash, AskPeer& askPeer)
    : mAskPeer(askPeer)
    , mApp(app)
    , mNumListRebuild(0)
    , mTimer(app)
    , mItemHash(hash)
    , mTryNextPeer(app.getMetrics().NewMeter(
          {"overlay", "item-fetcher", "next-peer"}, "item-fetcher"))
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

    CLOG(TRACE, "Overlay") << "tryNextPeer " << hexAbbrev(mItemHash)
                           << " last: "
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

        // move the peers that have the envelope to the back,
        // to be processed first
        for (auto const& p :
             mApp.getOverlayManager().getRandomAuthenticatedPeers())
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

        CLOG(TRACE, "Overlay")
            << "tryNextPeer " << hexAbbrev(mItemHash) << " attempt "
            << mNumListRebuild << " reset to #" << mPeersToAsk.size();
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
        if (mLastAskedPeer)
        {
            mTryNextPeer.Mark();
        }
        mLastAskedPeer = peer;
        CLOG(TRACE, "Overlay") << "Asking for " << hexAbbrev(mItemHash)
                               << " to " << peer->toString();
        mAskPeer(peer, mItemHash);
        nextTry = MS_TO_WAIT_FOR_FETCH_REPLY;
    }

    mTimer.expires_from_now(nextTry);
    mTimer.async_wait([this]() { this->tryNextPeer(); },
                      VirtualTimer::onFailureNoop);
}

void
Tracker::listen(const SCPEnvelope& env)
{
    mLastSeenSlotIndex = std::max(env.statement.slotIndex, mLastSeenSlotIndex);

    StellarMessage m;
    m.type(SCP_MESSAGE);
    m.envelope() = env;
    mWaitingEnvelopes.push_back(
        std::make_pair(sha256(xdr::xdr_to_opaque(m)), env));
}

void
Tracker::discard(const SCPEnvelope& env)
{
    auto matchEnvelope = [&env](std::pair<Hash, SCPEnvelope> const& x) {
        return x.second == env;
    };
    mWaitingEnvelopes.erase(std::remove_if(std::begin(mWaitingEnvelopes),
                                           std::end(mWaitingEnvelopes),
                                           matchEnvelope),
                            std::end(mWaitingEnvelopes));
}

void
Tracker::cancel()
{
    mTimer.cancel();
    mLastSeenSlotIndex = 0;
}
}
