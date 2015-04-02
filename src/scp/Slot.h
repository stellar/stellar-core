#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "scp/SCP.h"
#include "lib/json/json-forwards.h"

namespace stellar
{
std::string ballotToStr(SCPBallot const& ballot);
std::string envToStr(SCPEnvelope const& envelope);

class Node;

/**
 * The Slot object is in charge of maintaining the state of the SCP protocol
 * for a given slot index.
 */
class Slot
{
  public:
    // Constructor
    Slot(uint64 const& slotIndex, SCP* SCP);

    // Process a newly received envelope for this slot and update the state of
    // the slot accordingly. `cb` asynchronously returns whether the envelope
    // was validated or not. Must exclusively receive envelopes whose payload
    // type is STATEMENT
    void processEnvelope(SCPEnvelope const& envelope,
                         std::function<void(SCP::EnvelopeState)> const& cb);

    // Prepares a new ballot with the provided value for this slot. If the
    // value is less or equal to the current ballot value, and forceBump is
    // false, the current ballot counter is used. Otherwise a new ballot is
    // generated with an increased counter value.
    bool prepareValue(Value const& value, bool forceBump = false);

    size_t getStatementCount() const;

    void dumpInfo(Json::Value& ret);

  private:
    // bumps to the specified ballot
    void bumpToBallot(SCPBallot const& ballot);

    // Helper methods to generate a new envelopes
    SCPStatement createStatement(SCPStatementType const& type);
    SCPEnvelope createEnvelope(SCPStatement const& statement);

    // `attempt*` methods progress the slot to the specified state if it was
    // not already reached previously. They are in charge of emitting events
    // and envelopes. They are idempotent. `attemptPrepared` takes an extra
    // ballot argument as we can emit PREPARED messages for ballots different
    // than our current `mBallot`.
    void attemptPrepare();
    void attemptPrepared(SCPBallot const& ballot);
    void attemptCommit();
    void attemptCommitted();
    void attemptExternalize();

    // `is*` methods check if the specified state for the current slot has been
    // reached or not. They are called by `advanceSlot` and drive the call of
    // the `attempt*` methods. `isCommittedConfirmed` does not take a ballot
    // but a `Value` as it is determined across ballot rounds
    bool isPristine();
    bool isPrepared(SCPBallot const& ballot);
    bool isPreparedConfirmed(SCPBallot const& ballot);
    bool isCommitted(SCPBallot const& ballot);
    bool isCommittedConfirmed(Value const& value);

    // Retrieve all the statements of a given type for a given node
    std::vector<SCPStatement> getNodeStatements(uint256 const& nodeID,
                                                SCPStatementType const& type);

    // Helper method to compare two ballots
    int compareBallots(SCPBallot const& b1, SCPBallot const& b2);

    // `advanceSlot` can be called as many time as needed. It attempts to
    // advance the slot to a next state if possible given the current
    // knowledge of this node.
    void advanceSlot();

    const uint64 mSlotIndex;
    SCP* mSCP;

    // mBallot is the current ballot (monotonically increasing).
    SCPBallot mBallot;
    // mIsPristine is true while we never bump our ballot (mBallot invalid)
    bool mIsPristine;

    bool mHeardFromQuorum;
    bool mIsCommitted;
    bool mIsExternalized;

    bool mInAdvanceSlot;
    bool mRunAdvanceSlot;

    // mStatements keep track of all statements seen so far for this slot.
    // SCPBallot -> SCPStatementType -> uint256 -> SCPStatement
    struct StatementMap : public std::map<uint256, SCPStatement>
    {
    };
    struct StatementTypeMap : public std::map<SCPStatementType, StatementMap>
    {
    };
    std::map<SCPBallot, StatementTypeMap> mStatements;

    friend class Node;
};
}
