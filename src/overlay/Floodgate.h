#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "generated/StellarXDR.h"
#include "overlay/Peer.h"
#include <map>

/*
Keeps track of what peers have sent us which flood messages so we know who to
send to when we broadcast the messages in return

Transactions  (fullHash)
Prepare		(hash of envelope)
Aborted
Commit
Committed

*/

namespace stellar
{
class PeerMaster;

class FloodRecord
{
  public:
    typedef std::shared_ptr<FloodRecord> pointer;

    uint64_t mLedgerIndex;
    StellarMessage mMessage;
    std::vector<Peer::pointer> mPeersTold;

    FloodRecord(StellarMessage const& msg, uint64_t ledger,
                Peer::pointer peer);
};

class Floodgate
{
    std::map<uint256, FloodRecord::pointer> mFloodMap;
    Application& mApp;

  public:
    Floodgate(Application& app);
    // Floodgate will be cleared after every ledger close
    void clearBelow(uint64_t currentLedger);
    // returns true if this is a new record
    bool addRecord(StellarMessage const& msg, Peer::pointer fromPeer);

    void broadcast(StellarMessage const& msg);
};
}

