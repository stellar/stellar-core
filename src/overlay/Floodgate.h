#ifndef __FLOODGATE__
#define __FLOODGATE__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "generated/StellarXDR.h"
#include "overlay/Peer.h"
#include <map>

/*
Keeps track of what peers have sent us which flood messages so we know who to
send to when we broadcast the messages in return

Transactions  (txID)
Prepare		(sig I know they could be malleable but that doesn't
matter in this case the worse thing would be we would flood twice)
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
    vector<Peer::pointer> mPeersTold;

    FloodRecord(StellarMessage const& msg, uint64_t ledger,
                Peer::pointer peer);
};

class Floodgate
{
    std::map<uint256, FloodRecord::pointer> mFloodMap;

  public:
    // Floodgate will be cleared after every ledger close
    void clearBelow(uint64_t currentLedger);
    void addRecord(uint256 const& index,
                   StellarMessage const& msg, uint64_t ledgerIndex,
                   Peer::pointer peer);

    void broadcast(uint256 const& index, PeerMaster* peerMaster);
};
}

#endif
