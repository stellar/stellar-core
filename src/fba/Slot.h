#ifndef __FBA_SLOT__
#define __FBA_SLOT__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "fba/FBA.h"

namespace stellar 
{
using xdr::operator==;
using xdr::operator<;

class Node;

/**
 * The Slot object is in charge of maintaining the state of the FBA protocol
 * for a given slot index.
 */
class Slot
{
  public:
    // Constructor
    Slot(const uint64& slotIndex, FBA* FBA);

    // Process a newly received envelope for this slot and update the state of
    // the slot accordingly. Returns wether the envelope was validated or not.
    void processEnvelope(const FBAEnvelope& envelope,
                         std::function<void(bool)> const& cb = [] (bool) { });

    // Attempts a new value for this slot. If the value is compatible with the
    // current ballot, and forceBump is false, the current ballot is used.
    // Otherwise a new ballot is generated with an increased counter value.
    bool attemptValue(const Value& value, 
                      bool forceBump = false);

  private:
    // bumps to the specified ballot emitting a valueCancelled if needed
    void bumpToBallot(const FBABallot& ballot);

    // Helper methods to generate a new envelopes
    FBAStatement createStatement(const FBAStatementType& type);
    FBAEnvelope createEnvelope(const FBAStatement& statement);

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

    // Helper method to test if the filtered set of statements is a transitive
    // quorum *and* whether this transitive quorum is a quorum for the local
    // node.
    bool isQuorumTransitive(
        const std::map<uint256, FBAStatement>& statements,
        std::function<bool(const uint256&, const FBAStatement&)> const& filter =
          [] (const uint256&, const FBAStatement&) { return true; });
    // Helper method to test if the filtered set of statements is a v-blocking
    // set for the node specified by nodeID.
    bool isVBlocking(
        const std::map<uint256, FBAStatement>& statements,
        const uint256& nodeID,
        std::function<bool(const uint256&, const FBAStatement&)> const& filter =
          [] (const uint256&, const FBAStatement&) { return true; });

    // `is*` methods check if the specified state for the current slot has been
    // reached or not. They are called by `advanceSlot` and drive the call of
    // the `attempt*` methods. `isCommittedConfirmed` does not take a ballot
    // but a `Value` as it is determined across ballot rounds
    bool isPristine();
    bool isPrepared(const FBABallot& ballot);
    bool isPreparedConfirmed(const FBABallot& ballot);
    bool isCommitted(const FBABallot& ballot);
    bool isCommittedConfirmed(const Value& value);

    // Retrieve all the statements of a given type for a given node
    std::vector<FBAStatement> getNodeStatements(const uint256& nodeID,
                                                const FBAStatementType& type);

    // Helper method to compare two ballots
    int compareBallots(const FBABallot& b1, const FBABallot& b2);
                                          
    // `advanceSlot` can be called as many time as needed. It attempts to
    // advance the slot to a next state if possible given the current
    // knownledge of this node. 
    void advanceSlot();

    const uint64                                               mSlotIndex;
    FBA*                                                       mFBA;

    // mBallot is the current ballot (monotically increasing)
    FBABallot                                                  mBallot;

    bool                                                       mIsPristine;
    bool                                                       mIsCommitted;
    bool                                                       mIsExternalized;

    bool                                                       mInAdvanceSlot;
    bool                                                       mRunAdvanceSlot;

    // mEnvelopes keep track of all received and sent envelopes for this slot.
    std::map<FBABallot,
             std::map<FBAStatementType,
                      std::map<uint256, FBAStatement>>>        mStatements;

    friend class Node;
};

}

#endif
