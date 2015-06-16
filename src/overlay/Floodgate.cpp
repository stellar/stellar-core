// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/Floodgate.h"
#include "crypto/SHA.h"
#include "main/Application.h"
#include "overlay/OverlayManager.h"
#include "herder/Herder.h"

#include "medida/counter.h"
#include "medida/metrics_registry.h"
#include "xdrpp/marshal.h"

namespace stellar
{

Floodgate::FloodRecord::FloodRecord(StellarMessage const& msg, uint32_t ledger,
                                    Peer::pointer peer)
    : mLedgerSeq(ledger), mMessage(msg)
{
    if (peer)
        mPeersTold.push_back(peer);
}

Floodgate::Floodgate(Application& app)
    : mApp(app)
    , mFloodMapSize(
          app.getMetrics().NewCounter({"overlay", "memory", "flood-map"}))
    , mShuttingDown(false)
{
}

// remove old flood records
void
Floodgate::clearBelow(uint32_t currentLedger)
{
    for (auto it = mFloodMap.cbegin(); it != mFloodMap.cend();)
    {
        // give one ledger of leeway
        if (it->second->mLedgerSeq + 10 < currentLedger)
        {
            mFloodMap.erase(it++);
        }
        else
        {
            ++it;
        }
    }
    mFloodMapSize.set_count(mFloodMap.size());
}

bool
Floodgate::addRecord(StellarMessage const& msg, Peer::pointer peer)
{
    if (mShuttingDown)
    {
        return false;
    }
    Hash index = sha256(xdr::xdr_to_opaque(msg));
    auto result = mFloodMap.find(index);
    if (result == mFloodMap.end())
    { // we have never seen this message
        mFloodMap[index] = std::make_shared<FloodRecord>(
            msg, mApp.getHerder().getCurrentLedgerSeq(), peer);
        mFloodMapSize.set_count(mFloodMap.size());
        return true;
    }
    else
    {
        result->second->mPeersTold.push_back(peer);
        return false;
    }
}

// send message to anyone you haven't gotten it from
void
Floodgate::broadcast(StellarMessage const& msg, bool force)
{
    if (mShuttingDown)
    {
        return;
    }
    Hash index = sha256(xdr::xdr_to_opaque(msg));
    auto result = mFloodMap.find(index);
    if (result == mFloodMap.end() || force)
    { // no one has sent us this message
        FloodRecord::pointer record = std::make_shared<FloodRecord>(
            msg, mApp.getHerder().getCurrentLedgerSeq(), Peer::pointer());
        record->mPeersTold = mApp.getOverlayManager().getPeers();

        mFloodMap[index] = record;
        mFloodMapSize.set_count(mFloodMap.size());
        for (auto peer : mApp.getOverlayManager().getPeers())
        {
            if (peer->getState() == Peer::GOT_HELLO)
            {
                peer->sendMessage(msg);
                record->mPeersTold.push_back(peer);
            }
        }
    }
    else
    { // send it to people that haven't sent it to us
        std::vector<Peer::pointer>& peersTold = result->second->mPeersTold;
        for (auto peer : mApp.getOverlayManager().getPeers())
        {
            if (find(peersTold.begin(), peersTold.end(), peer) ==
                peersTold.end())
            {
                if (peer->getState() == Peer::GOT_HELLO)
                {
                    peer->sendMessage(msg);
                    peersTold.push_back(peer);
                }
            }
        }
    }
}

void
Floodgate::shutdown()
{
    mShuttingDown = true;
    mFloodMap.clear();
}
}
