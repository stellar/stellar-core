#ifndef __PEERMASTER__
#define __PEERMASTER__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "Peer.h"
#include "PeerDoor.h"
#include "overlay/ItemFetcher.h"
#include "overlay/Floodgate.h"
#include <vector>
#include <thread>
#include "generated/StellarXDR.h"
#include "overlay/OverlayGateway.h"
#include "overlay/PreferredPeers.h"
#include "util/Timer.h"

using namespace std;
/*
Maintain the set of peers we are connected to
*/
namespace stellar
{

class PeerMaster : public OverlayGateway
{
    Application& mApp;
    // peers we are connected to
    vector<Peer::pointer> mPeers;
    PeerDoor mDoor;
    QSetFetcher mQSetFetcher;
    PreferredPeers mPreferredPeers;

    void addConfigPeers();

    void tick();
    VirtualTimer mTimer;

  public:
    Floodgate mFloodGate;

    PeerMaster(Application& app);
    ~PeerMaster();

    //////// GATEWAY FUNCTIONS
    void ledgerClosed(LedgerPtr ledger);

    QuorumSet::pointer
    fetchQuorumSet(uint256 const& itemID, bool askNetwork)
    {
        return (mQSetFetcher.fetchItem(itemID, askNetwork));
    }
    void
    recvFloodedMsg(uint256 const& index,
                   StellarMessage const& msg, uint32_t ledgerIndex,
                   Peer::pointer peer)
    {
        mFloodGate.addRecord(index, msg, ledgerIndex, peer);
    }
    void
    doesntHaveQSet(uint256 const& index, Peer::pointer peer)
    {
        mQSetFetcher.doesntHave(index, peer);
    }

    void broadcastMessage(StellarMessage const& msg,
                          Peer::pointer peer);
    void recvQuorumSet(QuorumSet::pointer qset);
    //////

    void addPeer(Peer::pointer peer);
    void dropPeer(Peer::pointer peer);
    bool isPeerAccepted(Peer::pointer peer);

    Peer::pointer getRandomPeer();
    Peer::pointer getNextPeer(
        Peer::pointer peer); // returns NULL if the passed peer isn't found

    void broadcastMessage(uint256 const& msgID);
    void broadcastMessage(StellarMessage const& msg,
                          vector<Peer::pointer> const& skip);

    static void createTable(Database &db);
    static const char *kSQLCreateStatement;
};
}

#endif
