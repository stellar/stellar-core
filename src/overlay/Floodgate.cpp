// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "Floodgate.h"
#include "overlay/PeerMaster.h"

namespace stellar
{

FloodRecord::FloodRecord(StellarMessage const& msg, uint32_t ledger,
                         Peer::pointer peer)
{
    mMessage = msg;
    mLedgerIndex = ledger;
    mPeersTold.push_back(peer);
}

// remove old flood records
void
Floodgate::clearBelow(uint32_t currentLedger)
{
    for (auto it = mFloodMap.cbegin(); it != mFloodMap.cend();)
    {
        if (it->second->mLedgerIndex < currentLedger)
        {
            mFloodMap.erase(it++);
        }
        else
        {
            ++it;
        }
    }
}

void
Floodgate::addRecord(uint256 const& index,
                     StellarMessage const& msg,
                     uint32_t ledgerIndex, Peer::pointer peer)
{
    mFloodMap[index] = std::make_shared<FloodRecord>(msg, ledgerIndex, peer);
}

void
Floodgate::broadcast(uint256 const& index, PeerMaster* peerMaster)
{
    auto result = mFloodMap.find(index);
    if (result != mFloodMap.end())
    {
        peerMaster->broadcastMessage(result->second->mMessage,
                                     result->second->mPeersTold);
    }
}
}
