// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/Floodgate.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "herder/Herder.h"
#include "main/Application.h"
#include "medida/counter.h"
#include "medida/metrics_registry.h"
#include "overlay/OverlayManager.h"
#include "util/Logging.h"
#include "util/XDROperators.h"
#include "xdrpp/marshal.h"
#include <Tracy.hpp>

namespace stellar
{
Floodgate::FloodRecord::FloodRecord(StellarMessage const& msg, uint32_t ledger,
                                    Peer::pointer peer)
    : mLedgerSeq(ledger), mMessage(msg)
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
    , mShuttingDown(false)
{
}

// remove old flood records
void
Floodgate::clearBelow(uint32_t currentLedger)
{
    ZoneScoped;
    for (auto it = mFloodMap.cbegin(); it != mFloodMap.cend();)
    {
        // give one ledger of leeway
        if (it->second->mLedgerSeq + 10 < currentLedger)
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
Floodgate::addRecord(StellarMessage const& msg, Peer::pointer peer, Hash& index)
{
    ZoneScoped;
    index = sha256(xdr::xdr_to_opaque(msg));
    if (mShuttingDown)
    {
        return false;
    }
    auto result = mFloodMap.find(index);
    if (result == mFloodMap.end())
    { // we have never seen this message
        mFloodMap[index] = std::make_shared<FloodRecord>(
            msg, mApp.getHerder().getCurrentLedgerSeq(), peer);
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
void
Floodgate::broadcast(StellarMessage const& msg, bool force)
{
    ZoneScoped;
    if (mShuttingDown)
    {
        return;
    }
    Hash index = sha256(xdr::xdr_to_opaque(msg));

    FloodRecord::pointer fr;
    auto result = mFloodMap.find(index);
    if (result == mFloodMap.end() || force)
    { // no one has sent us this message / start from scratch
        fr = std::make_shared<FloodRecord>(
            msg, mApp.getHerder().getCurrentLedgerSeq(), Peer::pointer());
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

    bool log = true;
    for (auto peer : peers)
    {
        assert(peer.second->isAuthenticated());
        if (peersTold.find(peer.second->toString()) == peersTold.end())
        {
            mSendFromBroadcast.Mark();
            peer.second->sendMessage(msg, log);
            peersTold.insert(peer.second->toString());
            log = false;
        }
    }
    CLOG(TRACE, "Overlay") << "broadcast " << hexAbbrev(index) << " told "
                           << peersTold.size();
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

void
Floodgate::updateRecord(StellarMessage const& oldMsg,
                        StellarMessage const& newMsg)
{
    ZoneScoped;
    Hash oldHash = sha256(xdr::xdr_to_opaque(oldMsg));
    Hash newHash = sha256(xdr::xdr_to_opaque(newMsg));

    auto oldIter = mFloodMap.find(oldHash);
    if (oldIter != mFloodMap.end())
    {
        auto record = oldIter->second;
        record->mMessage = newMsg;

        mFloodMap.erase(oldIter);
        mFloodMap.emplace(newHash, record);
    }
}
}
