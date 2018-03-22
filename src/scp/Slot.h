#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "BallotProtocol.h"
#include "LocalNode.h"
#include "NominationProtocol.h"
#include "lib/json/json-forwards.h"
#include "scp/SCP.h"
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <utility>

namespace stellar
{
class Node;

/**
 * The Slot object is in charge of maintaining the state of the SCP protocol
 * for a given slot index.
 */
class Slot : public std::enable_shared_from_this<Slot>
{
    const uint64 mSlotIndex; // the index this slot is tracking
    SCP& mSCP;

    BallotProtocol mBallotProtocol;
    NominationProtocol mNominationProtocol;

    // keeps track of all statements seen so far for this slot.
    // it is used for debugging purpose
    struct HistoricalStatement
    {
        time_t mWhen;
        SCPStatement mStatement;
        bool mValidated;
    };

    std::vector<HistoricalStatement> mStatementsHistory;

    // true if the Slot was fully validated
    bool mFullyValidated;

  public:
    Slot(uint64 slotIndex, SCP& SCP);

    uint64
    getSlotIndex() const
    {
        return mSlotIndex;
    }

    SCP&
    getSCP()
    {
        return mSCP;
    }

    SCPDriver&
    getSCPDriver()
    {
        return mSCP.getDriver();
    }

    SCPDriver const&
    getSCPDriver() const
    {
        return mSCP.getDriver();
    }

    BallotProtocol&
    getBallotProtocol()
    {
        return mBallotProtocol;
    }

    Value const& getLatestCompositeCandidate();

    // returns the latest messages the slot emitted
    std::vector<SCPEnvelope> getLatestMessagesSend() const;

    // forces the state to match the one in the envelope
    // this is used when rebuilding the state after a crash for example
    void setStateFromEnvelope(SCPEnvelope const& e);

    // returns the latest messages known for this slot
    std::vector<SCPEnvelope> getCurrentState() const;

    // returns messages that helped this slot externalize
    std::vector<SCPEnvelope> getExternalizingState() const;

    // records the statement in the historical record for this slot
    void recordStatement(SCPStatement const& st);

    // Process a newly received envelope for this slot and update the state of
    // the slot accordingly.
    // self: set to true when node wants to record its own messages (potentially
    // triggering more transitions)
    SCP::EnvelopeState processEnvelope(SCPEnvelope const& envelope, bool self);

    bool abandonBallot();

    // bumps the ballot based on the local state and the value passed in:
    // in prepare phase, attempts to take value
    // otherwise, no-ops
    // force: when true, always bumps the value, otherwise only bumps
    // the state if no value was prepared
    bool bumpState(Value const& value, bool force);

    // attempts to nominate a value for consensus
    bool nominate(Value const& value, Value const& previousValue,
                  bool timedout);

    void stopNomination();

    bool isFullyValidated() const;
    void setFullyValidated(bool fullyValidated);

    // returns if a node is in the quorum originating at the local node
    SCP::TriBool isNodeInQuorum(NodeID const& node);

    // ** status methods

    size_t
    getStatementCount() const
    {
        return mStatementsHistory.size();
    }

    // returns information about the local state in JSON format
    // including historical statements if available
    Json::Value getJsonInfo();

    // returns information about the quorum for a given node
    Json::Value getJsonQuorumInfo(NodeID const& id, bool summary);

    // returns the hash of the QuorumSet that should be downloaded
    // with the statement.
    // note: the companion hash for an EXTERNALIZE statement does
    // not match the hash of the QSet, but the hash of commitQuorumSetHash
    static Hash getCompanionQuorumSetHashFromStatement(SCPStatement const& st);

    // returns the values associated with the statement
    static std::vector<Value> getStatementValues(SCPStatement const& st);

    // returns the QuorumSet that should be used for a node given the
    // statement (singleton for externalize)
    SCPQuorumSetPtr getQuorumSetFromStatement(SCPStatement const& st);

    // wraps a statement in an envelope (sign it, etc)
    SCPEnvelope createEnvelope(SCPStatement const& statement);

    // ** federated agreement helper functions

    // returns true if the statement defined by voted and accepted
    // should be accepted
    bool federatedAccept(StatementPredicate voted, StatementPredicate accepted,
                         std::map<NodeID, SCPEnvelope> const& envs);
    // returns true if the statement defined by voted
    // is ratified
    bool federatedRatify(StatementPredicate voted,
                         std::map<NodeID, SCPEnvelope> const& envs);

    std::shared_ptr<LocalNode> getLocalNode();

    enum timerIDs
    {
        NOMINATION_TIMER = 0,
        BALLOT_PROTOCOL_TIMER = 1
    };

  protected:
    std::vector<SCPEnvelope> getEntireCurrentState();
    friend class TestSCP;
};
}
