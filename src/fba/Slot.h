#ifndef __FBA_SLOT__
#define __FBA_SLOT__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "fba/FBA.h"

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

  private:

    void doPrepare();
    void doPrepared();
    void doCommit();
    void doCommitted();
    void doExternalize();

    bool isQuorumTransitive(
        const FBAStatementType& type,
        const uint256& nodeID,
        std::function<bool(const FBAStatement&)> const& filter);
    bool isVBlocking(
        const FBAStatementType& type,
        const uint256& nodeID,
        std::function<bool(const FBAStatement&)> const& filter);

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
