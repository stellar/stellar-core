#ifndef __OVERLAYGATEWAY__
#define __OVERLAYGATEWAY__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "generated/StellarXDR.h"

/*
Public interface to the Overlay module

Maintains the connection to the Stellar network
Handles flooding the appropriate messages to the rest of the world
There are 3 classes of messages:
-Messages to a particular Peer that want a response:
    Hello,  getPeers
-Messages that are flooded to the network:
    transaction, prepare, prepared, etc...
-Messages that you want a response from some peer but you don't care which.
Should keep rotating peers till you get an answer:
    getTxSet, getQuorumSet, getHistory, getDelta, ...


*/

#include "overlay/Peer.h"
#include "fba/FBA.h"

namespace stellar
{
class Ledger;
typedef std::shared_ptr<Ledger> LedgerPtr;
typedef std::shared_ptr<FBAQuorumSet> FBAQuorumSetPtr;

class OverlayGateway
{
  public:
    // called by Ledger
    virtual void ledgerClosed(LedgerPtr ledger) = 0;
    virtual void fetchDelta(uint256 const& oldLedgerHash,
                            uint32_t oldLedgerSeq) = 0;

    // called by TxHerder
    virtual void broadcastMessage(uint256 const& messageID) = 0;

    // called by FBA
    virtual FBAQuorumSetPtr fetchFBAQuorumSet(uint256 const& itemID,
                                              bool askNetwork) = 0;

    // called internally and by FBA
    virtual void broadcastMessage(StellarMessage const& msg,
                                  Peer::pointer peer) = 0;
    // virtual Item trackDownItem(uint256 itemID,
    // StellarMessage& msg) = 0;  needs to return Item this is the
    // issue
    // virtual void stopTrackingItem(uint256 itemID) = 0;

    // called internally
    virtual void recvFBAQuorumSet(FBAQuorumSetPtr qset) = 0;
    virtual void doesntHaveQSet(uint256 const& index,
                                Peer::pointer peer) = 0;
    virtual void recvFloodedMsg(uint256 const& index,
                                StellarMessage const& msg,
                                uint32_t ledgerIndex, Peer::pointer peer) = 0;
    virtual Peer::pointer getRandomPeer() = 0;
    virtual Peer::pointer getNextPeer(
        Peer::pointer peer) = 0; // returns NULL if the passed peer isn't found
};
}

#endif
