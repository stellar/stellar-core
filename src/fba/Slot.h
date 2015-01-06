#ifndef __FBA_SLOT__
#define __FBA_SLOT__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "fba/FBA.h"

#define FBA_SLOT_MAX_COUNTER 0xffffffff

namespace stellar 
{
/**
 * The Slot object is in charge of maintaining the state of the FBA protocol
 * for a given slot index.
 */
class Slot
{
  public:
    // Constructor
    Slot(const uint32& slotIndex,
         FBA* FBA);

    void processEnvelope(const FBAEnvelope& envelope);
    bool attemptValue(const uint256& valueHash);

  private:
    void attemptPrepare();
    void attemptPrepared();
    void attemptCommit();
    void attemptCommitted();
    void attemptExternalize();

    bool nodeHasQuorum(const uint256& nodeID,
                       const uint256& qSetHash,
                       const std::vector<uint256>& nodeSet);
    bool nodeIsVBlocking(const uint256& nodeID,
                         const uint256& qSetHash,
                         const std::vector<uint256>& nodeSet);

    bool isQuorumTransitive(
        const FBAStatementType& type,
        const uint256& nodeID,
        std::function<bool(const FBAEnvelope&)> const& filter =
          [] (const FBAEnvelope&) { return true; });
    bool isVBlocking(
        const FBAStatementType& type,
        const uint256& nodeID,
        std::function<bool(const FBAEnvelope&)> const& filter =
          [] (const FBAEnvelope&) { return true; });

    bool isNull();
    bool isPrepared();
    bool isPreparedConfirmed();
    bool isCommitted();
    bool isCommittedConfirmed();

    void advanceSlot();

    const uint32&                                              mSlotIndex;
    FBA*                                                       mFBA;
    FBABallot                                                  mBallot;
    std::map<FBAStatementType, std::map<uint256, FBAEnvelope>> mEnvelopes;
};

}

#endif
