#ifndef __FBA_SLOT__
#define __FBA_SLOT__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "fba/FBA.h"

#define FBA_SLOT_MAX_COUNTER 0xffffffff

namespace stellar 
{
class Node;

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

    // Process a newly received envelope for this slot and update the state of
    // the slot accordingly.
    void processEnvelope(const FBAEnvelope& envelope);

    // Attempts a new value for this slot. If the value is compatible with the
    // current ballot, and forceBump is false, the current ballot is used.
    // Otherwise a new ballot is generated with an increased counter value.
    bool attemptValue(const Hash& valueHash, 
                      const Hash& evidence,
                      bool forceBump = false);

  private:
    // Helper methods to generate a new envelopes
    FBAEnvelope createEnvelope(const FBAStatementType& type);
    std::string envToStr(const FBAEnvelope& envelope);

    // Signature and verificatio methods
    void signEnvelope(FBAEnvelope& envelope);
    bool verifyEnvelope(const FBAEnvelope& envelope);

    // `attempt*` methods progress the slot to the specified state if it was
    // not already reached previously. They are in charge of emitting events
    // and envelopes. They are indempotent.
    void attemptPrepare();
    void attemptPrepared();
    void attemptCommit();
    void attemptCommitted();
    void attemptExternalize();

    // Helper functions to test a nodeSet against a specifed quorum set for a
    // given node.
    bool nodeHasQuorum(const uint256& nodeID,
                       const Hash& qSetHash,
                       const std::vector<uint256>& nodeSet);
    bool nodeIsVBlocking(const uint256& nodeID,
                         const Hash& qSetHash,
                         const std::vector<uint256>& nodeSet);

    // Helper methods to test if we have a transitive quorum or a v-blocking
    // set of nodes which have pledge the statements of the specified type for
    // a given node specified by nodeID. The filter function allows to restrict
    // the set of nodes to a subset matching a certain criteria.
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

    // `is*` methods check if the specified state for the current slot has been
    // reached or not. They are called by `advanceSlot` and drive the call of
    // the `attempt*` methods.
    bool isNull();
    bool isPrepared();
    bool isPreparedConfirmed();
    bool isCommitted();
    bool isCommittedConfirmed();

    // `advanceSlot` can be called as many time as needed. It attempts to
    // advance the slot to a next state if possible given the current
    // knownledge of this node. 
    void advanceSlot();

    const uint32&                                              mSlotIndex;
    FBA*                                                       mFBA;

    FBABallot                                                  mBallot;
    Hash                                                       mEvidence;

    bool                                                       mInAdvanceSlot;
    bool                                                       mRunAdvanceSlot;

    std::vector<FBABallot>                                     mPledgedCommit;
    std::map<Hash, FBABallot>                                  mPrepared;

    std::map<FBAStatementType, std::map<uint256, FBAEnvelope>> mEnvelopes;

    friend class Node;
};

}

#endif
