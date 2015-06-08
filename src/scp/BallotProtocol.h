#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <memory>
#include <functional>
#include <string>
#include <set>
#include <utility>
#include "scp/SCP.h"
#include "lib/json/json-forwards.h"

namespace stellar
{
class Node;
class Slot;

// used to filter statements
typedef std::function<bool(uint256 const& nodeID, SCPStatement const& st)>
    StatementPredicate;

/**
 * The Slot object is in charge of maintaining the state of the SCP protocol
 * for a given slot index.
 */
class BallotProtocol
{
    Slot& mSlot;

    bool mHeardFromQuorum;

    // state tracking members
    enum SCPPhase
    {
        SCP_PHASE_PREPARE,
        SCP_PHASE_CONFIRM,
        SCP_PHASE_EXTERNALIZE,
        SCP_PHASE_NUM
    };
    // human readable names matching SCPPhase
    static const char* phaseNames[];

    std::unique_ptr<SCPBallot> mCurrentBallot;         // b
    std::unique_ptr<SCPBallot> mPrepared;              // p
    std::unique_ptr<SCPBallot> mPreparedPrime;         // p'
    std::unique_ptr<SCPBallot> mConfirmedPrepared;     // P
    std::unique_ptr<SCPBallot> mCommit;                // c
    std::map<uint256, SCPStatement> mLatestStatements; // M
    SCPPhase mPhase;                                   // Phi

    std::unique_ptr<SCPEnvelope>
        mLastEnvelope; // last envelope emitted by this node

  public:
    BallotProtocol(Slot& slot);

    // Process a newly received envelope for this slot and update the state of
    // the slot accordingly. `cb` asynchronously returns whether the envelope
    // was validated or not. Must exclusively receive envelopes whose payload
    // type is STATEMENT
    SCP::EnvelopeState processEnvelope(SCPEnvelope const& envelope);

    bool abandonBallot();

    // bumps the ballot based on the local state and the value passed in:
    // in prepare phase, attempts to take value
    // otherwise, no-ops
    bool bumpState(Value const& value);

    // ** status methods

    // returns information about the local state in JSON format
    // including historical statements if available
    void dumpInfo(Json::Value& ret);

    // returns the hash of the QuorumSet that should be downloaded
    // with the statement.
    // note: the companion hash for an EXTERNALIZE statement does
    // not match the hash of the QSet, but the hash of commitQuorumSetHash
    static Hash getCompanionQuorumSetHashFromStatement(SCPStatement const& st);

    // helper function to retrieve b for PREPARE, P for CONFIRM or
    // c for EXTERNALIZE messages
    static SCPBallot getWorkingBallot(SCPStatement const& st);

  private:
    // attempts to make progress using `ballot` as a hint
    void advanceSlot(SCPBallot const& ballot);

    bool attemptPrepare(SCPBallot const& ballot);

    // `attempt*` methods progress the slot to the specified state if it was
    // not already reached previously:
    //   * They are in charge of emitting events and envelopes.
    //   * Their parameter "ballot" may be different than the local state,
    //     in which case they will switch to it if needed
    //   * They are idempotent.
    //   * returns true if the state was updated.

    // `is*` methods check if the specified state for the current slot has been
    // reached or not. They are called by `advanceSlot` and drive the call of
    // the `attempt*` methods.

    // step 1 and 4 from the SCP paper
    // ballot is the candidate to record as 'prepared'
    bool isPreparedAccept(SCPBallot const& ballot);
    bool attemptPreparedAccept(SCPBallot const& ballot);

    // step 2 from the SCP paper
    // ballot is the candidate to record as 'confirmed prepared'
    bool isPreparedConfirmed(SCPBallot const& ballot);
    bool attemptPreparedConfirmed(SCPBallot const& ballot);

    // step 3 and 5 from the SCP paper
    // ballot is used as a hint to find compatible ballots that the instance
    // should accept commit
    // on success, sets (outLow, outHigh) to the new values for (c, P)
    bool isAcceptCommit(SCPBallot const& ballot, SCPBallot& outLow,
                        SCPBallot& outHigh);
    bool attemptAcceptCommit(SCPBallot const& acceptCommitLow,
                             SCPBallot const& acceptCommitHigh);

    // step 6 from the SCP paper
    // ballot is used as a hint to find compatible ballots that can be
    // ratified commit.
    // on sucess, sets (outLow, outHigh) to the new values for (c, P)
    bool isConfirmCommit(SCPBallot const& ballot, SCPBallot& outLow,
                         SCPBallot& outHigh);
    bool attemptConfirmCommit(SCPBallot const& acceptCommitLow,
                              SCPBallot const& acceptCommitHigh);

    // An interval is [low,high] represented as a pair
    using Interval = std::pair<uint32, uint32>;

    // helper function to find a contiguous range 'candidate' that satisfies the
    // predicate.
    // 'candidate' can have an initial value to extend or be set to (0,0)
    // updates 'candidate' (or leave it unchanged)
    void findExtendedInterval(Interval& candidate,
                              std::set<Interval> const& boundaries,
                              std::function<bool(Interval const&)> pred);

    // constructs the set boundaries compatible with the ballot
    std::set<Interval>
    getCommitBoundariesFromStatements(SCPBallot const& ballot);

    // ** helper predicates that evaluate if a statement satisfies
    // a certain property

    // is ballot prepared by st
    bool hasPreparedBallot(SCPBallot const& ballot, uint256 const& nodeID,
                           SCPStatement const& st);

    // returns true if the statement commits the ballot in the range 'check'
    bool commitPredicate(SCPBallot const& ballot, Interval const& check,
                         uint256 const&, SCPStatement const& st);

    // ** Helper methods to compare two ballots

    // ballot comparison (ordering)
    int compareBallots(std::unique_ptr<SCPBallot> const& b1,
                       std::unique_ptr<SCPBallot> const& b2);
    int compareBallots(SCPBallot const& b1, SCPBallot const& b2);

    // b1 ~ b2
    bool areBallotsCompatible(SCPBallot const& b1, SCPBallot const& b2);

    // b1 <= b2 && b1 !~ b2
    bool areBallotsLessAndIncompatible(SCPBallot const& b1,
                                       SCPBallot const& b2);
    // b1 <= b2 && b1 ~ b2
    bool areBallotsLessAndCompatible(SCPBallot const& b1, SCPBallot const& b2);

    // ** statement helper functions

    // returns true if the statement is newer than the one we know about
    // for a given node.
    bool isNewerStatement(uint256 const& nodeID, SCPStatement const& st);

    // returns true if st is newer than oldst
    bool isNewerStatement(SCPStatement const& oldst, SCPStatement const& st);

    // basic sanity check on statement
    bool isStatementSane(SCPStatement const& st);

    // records the statement in the state machine
    void recordStatement(SCPStatement const& env);

    // ** State related methods

    // helper function that updates the current ballot
    // this is the lowest level method to update the current ballot and as
    // such doesn't do any validation
    void bumpToBallot(SCPBallot const& ballot);

    // switch the local node to the given ballot's value
    // with the assumption that the ballot is more recent than the one
    // we have.
    bool updateCurrentValue(SCPBallot const& ballot);

    // emits a statement reflecting the nodes's current state
    // and attempts to make progress
    void emitCurrentStateStatement();

    // verifies that the internal state is consistent
    void checkInvariants();

    // create a statement of the given type using the state in node
    SCPStatement createStatement(SCPStatementType const& type);

    // returns a string representing the slot's state
    // used for log lines
    std::string getLocalState() const;

    std::shared_ptr<LocalNode> getLocalNode();

  protected:
    bool federatedAccept(StatementPredicate voted, StatementPredicate accepted);
    bool federatedRatify(StatementPredicate voted);
};
}
