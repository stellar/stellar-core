#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "fba/FBA.h"

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
    Slot(const uint64& slotIndex, FBA* FBA);

    // Process a newly received envelope for this slot and update the state of
    // the slot accordingly. `cb` asynchronously returns wether the envelope
    // was validated or not. Must exclusively receive envelopes whose payload
    // type is STATEMENT
    void processEnvelope(const FBAEnvelope& envelope,
                         std::function<void(FBA::EnvelopeState)> const& cb);

    // Prepares a new ballot with the provided value for this slot. If the
    // value is less or equal to the current ballot value, and forceBump is
    // false, the current ballot counter is used. Otherwise a new ballot is
    // generated with an increased counter value.
    bool prepareValue(const Value& value, 
                      bool forceBump = false);

  private:
    // bumps to the specified ballot
    void bumpToBallot(const FBABallot& ballot);

    // Helper methods to generate a new envelopes
    FBAStatement createStatement(const FBAStatementType& type);
    FBAEnvelope createEnvelope(const FBAStatement& statement);

    // `attempt*` methods progress the slot to the specified state if it was
    // not already reached previously. They are in charge of emitting events
    // and envelopes. They are indempotent. `attemptPrepared` takes an extra
    // ballot argument as we can emit PREPARED messages for ballots different
    // than our current `mBallot`.
    void attemptPrepare();
    void attemptPrepared(const FBABallot& ballot);
    void attemptCommit();
    void attemptCommitted();
    void attemptExternalize();

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

    const uint64                                            mSlotIndex;
    FBA*                                                    mFBA;

    // mBallot is the current ballot (monotically increasing). 
    FBABallot                                               mBallot;
    // mIsPristine is true while we never bump our ballot (mBallot invalid)
    bool                                                    mIsPristine;

    bool                                                    mHeardFromQuorum;
    bool                                                    mIsCommitted;
    bool                                                    mIsExternalized;

    bool                                                    mInAdvanceSlot;
    bool                                                    mRunAdvanceSlot;

    // mStatements keep track of all statements seen so far for this slot.
    // FBABallot -> FBAStatementType -> uint256 -> FBAStatement
    struct StatementMap : 
        public std::map<uint256, FBAStatement> {};
    struct StatementTypeMap : 
        public std::map<FBAStatementType, StatementMap> {};
    std::map<FBABallot, StatementTypeMap>                   mStatements;

    friend class Node;
};

}

