// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/Floodgate.h"
#include "crypto/BLAKE2.h"
#include "crypto/Hex.h"
#include "herder/Herder.h"
#include "main/Application.h"
#include "medida/counter.h"
#include "medida/metrics_registry.h"
#include "overlay/OverlayManager.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include <Tracy.hpp>
#include <fmt/format.h>

namespace stellar
{
Floodgate::FloodRecord::FloodRecord(uint32_t ledger, Peer::pointer peer)
    : mLedgerSeq(ledger)
{
    if (peer)
        mPeersTold.insert(peer->toString());
}

Floodgate::Floodgate(Application& app)
    : mApp(app)
    , mFloodMapSize(
          app.getMetrics().NewCounter({"overlay", "memory", "flood-known"}))
    , mSendFromBroadcast(app.getMetrics().NewMeter(
          {"overlay", "flood", "broadcast"}, "message"))
    , mMessagesAdvertised(app.getMetrics().NewMeter(
          {"overlay", "flood", "advertised"}, "message"))
    , mShuttingDown(false)
{
}

// remove old flood records
void
Floodgate::clearBelow(uint32_t maxLedger)
{
    ZoneScoped;
    for (auto it = mFloodMap.cbegin(); it != mFloodMap.cend();)
    {
        if (it->second->mLedgerSeq < maxLedger)
        {
            it = mFloodMap.erase(it);
        }
        else
        {
            ++it;
        }
    }
    mFloodMapSize.set_count(mFloodMap.size());
}

bool
Floodgate::addRecord(StellarMessage const& msg, Peer::pointer peer,
                     Hash const& index)
{
    ZoneScoped;
    if (mShuttingDown)
    {
        return false;
    }
    auto result = mFloodMap.find(index);
    if (result == mFloodMap.end())
    { // we have never seen this message
        mFloodMap[index] = std::make_shared<FloodRecord>(
            mApp.getHerder().trackingConsensusLedgerIndex(), peer);
        mFloodMapSize.set_count(mFloodMap.size());
        TracyPlot("overlay.memory.flood-known",
                  static_cast<int64_t>(mFloodMap.size()));
        return true;
    }
    else
    {
        result->second->mPeersTold.insert(peer->toString());
        return false;
    }
}

// send message to anyone you haven't gotten it from
bool
Floodgate::broadcast(std::shared_ptr<StellarMessage const> msg,
                     std::optional<Hash> const& hash,
                     uint32_t minOverlayVersion)
{
    ZoneScoped;
    if (mShuttingDown)
    {
        return false;
    }
    if (msg->type() == TRANSACTION)
    {
        // Must pass a hash when broadcasting transactions.
        releaseAssert(hash.has_value());
    }
    Hash index = xdrBlake2(*msg);

    FloodRecord::pointer fr;
    auto result = mFloodMap.find(index);
    if (result == mFloodMap.end())
    { // no one has sent us this message / start from scratch
        fr = std::make_shared<FloodRecord>(
            mApp.getHerder().trackingConsensusLedgerIndex(), Peer::pointer());
        mFloodMap[index] = fr;
        mFloodMapSize.set_count(mFloodMap.size());
    }
    else
    {
        fr = result->second;
    }
    // send it to people that haven't sent it to us
    auto& peersTold = fr->mPeersTold;

    // make a copy, in case peers gets modified
    auto peers = mApp.getOverlayManager().getAuthenticatedPeers();

    bool broadcasted = false;
    for (auto peer : peers)
    {
        // Assert must hold since only main thread is allowed to modify
        // authenticated peers and peer state during drop
        peer.second->assertAuthenticated();
        if (peer.second->getRemoteOverlayVersion() < minOverlayVersion)
        {
            // Skip peers running overlay versions that are older than
            // `minOverlayVersion`.
            continue;
        }

        bool pullMode = msg->type() == TRANSACTION;

        if (peersTold.insert(peer.second->toString()).second)
        {
            if (pullMode)
            {
                if (peer.second->sendAdvert(hash.value()))
                {
                    mMessagesAdvertised.Mark();
                }
            }
            else
            {
                mSendFromBroadcast.Mark();

                if (msg->type() == SCP_MESSAGE)
                {
                    peer.second->sendMessage(msg, !broadcasted);
                }
                else
                {
                    // This is an async operation, and peer might get dropped by
                    // the time we actually try to send the message. This is
                    // fine, as sendMessage will just be a no-op in that case
                    std::weak_ptr<Peer> weak(
                        std::static_pointer_cast<Peer>(peer.second));
                    mApp.postOnMainThread(
                        [msg, weak, log = !broadcasted]() {
                            auto strong = weak.lock();
                            if (strong)
                            {
                                strong->sendMessage(msg, log);
                            }
                        },
                        fmt::format(FMT_STRING("broadcast to {}"),
                                    peer.second->toString()));
                }
            }
            broadcasted = true;
        }
    }
    CLOG_TRACE(Overlay, "broadcast {} told {}", hexAbbrev(index),
               peersTold.size());
    return broadcasted;
}

std::set<Peer::pointer>
Floodgate::getPeersKnows(Hash const& h)
{
    std::set<Peer::pointer> res;
    auto record = mFloodMap.find(h);
    if (record != mFloodMap.end())
    {
        auto& ids = record->second->mPeersTold;
        auto const& peers = mApp.getOverlayManager().getAuthenticatedPeers();
        for (auto& p : peers)
        {
            if (ids.find(p.second->toString()) != ids.end())
            {
                res.insert(p.second);
            }
        }
    }
    return res;
}

void
Floodgate::shutdown()
{
    mShuttingDown = true;
    mFloodMap.clear();
}

void
Floodgate::forgetRecord(Hash const& h)
{
    mFloodMap.erase(h);
}
}
