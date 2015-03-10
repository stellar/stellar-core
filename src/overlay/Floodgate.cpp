// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "Floodgate.h"
#include "overlay/PeerMaster.h"
#include "xdrpp/marshal.h"
#include "crypto/SHA.h"
#include "ledger/LedgerMaster.h"
#include "main/Application.h"

namespace stellar
{

FloodRecord::FloodRecord(StellarMessage const& msg, uint32_t ledger, Peer::pointer peer)
    : mLedgerSeq(ledger), mMessage(msg)
{
    if(peer) mPeersTold.push_back(peer);
}

Floodgate::Floodgate(Application& app) : mApp(app)
{

}

// remove old flood records
void
Floodgate::clearBelow(uint32_t currentLedger)
{
    for (auto it = mFloodMap.cbegin(); it != mFloodMap.cend();)
    {
        // give one ledger of leeway
        if (it->second->mLedgerSeq+1 < currentLedger)  
        {
            mFloodMap.erase(it++);
        }
        else
        {
            ++it;
        }
    }
}

bool
Floodgate::addRecord(StellarMessage const& msg, Peer::pointer peer)
{
    Hash index= sha256(xdr::xdr_to_msg(msg));
    auto result = mFloodMap.find(index);
    if(result == mFloodMap.end())
    { // we have never seen this message
        mFloodMap[index] = std::make_shared<FloodRecord>(msg, 
            mApp.getLedgerMaster().getLedgerNum(), peer);
        return true;
    }else
    { 
        result->second->mPeersTold.push_back(peer);
        return false;
    }
}

// send message to anyone you haven't gotten it from
void Floodgate::broadcast(StellarMessage const& msg,bool force)
{
    Hash index = sha256(xdr::xdr_to_msg(msg));
    auto result = mFloodMap.find(index);
    if(result == mFloodMap.end() || force)
    {  // no one has sent us this message
        FloodRecord::pointer record = std::make_shared<FloodRecord>(msg,
            mApp.getLedgerMaster().getLedgerNum(), Peer::pointer() );
        record->mPeersTold = mApp.getPeerMaster().getPeers();

        mFloodMap[index]= record;
        for(auto peer : mApp.getPeerMaster().getPeers())
        {
            if(peer->getState() == Peer::GOT_HELLO)
            {
                peer->sendMessage(msg);
                record->mPeersTold.push_back(peer);
            } 
        }
    } else
    { // send it to people that haven't sent it to us
        std::vector<Peer::pointer>& peersTold = result->second->mPeersTold;
        for(auto peer : mApp.getPeerMaster().getPeers())
        {
            if(find(peersTold.begin(), peersTold.end(), peer) == peersTold.end())
            {
                if(peer->getState() == Peer::GOT_HELLO)
                {
                    peer->sendMessage(msg);
                    peersTold.push_back(peer);
                }
            }
        }
    }
}
}
