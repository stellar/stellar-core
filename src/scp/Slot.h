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
#include "BallotProtocol.h"

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

    // keeps track of all statements seen so far for this slot.
    // it is used for debugging purpose
    std::vector<SCPStatement> mStatementsHistory;

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

    BallotProtocol&
    getBallotProtocol()
    {
        return mBallotProtocol;
    }

    // records the statement in the historical record for this slot
    void recordStatement(SCPStatement const& st);

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

    size_t
    getStatementCount() const
    {
        return mStatementsHistory.size();
    }

    // returns information about the local state in JSON format
    // including historical statements if available
    void dumpInfo(Json::Value& ret);

    // helper function to retrieve b for PREPARE, P for CONFIRM or
    // c for EXTERNALIZE messages
    static SCPBallot getWorkingBallot(SCPStatement const& st);

    // returns the hash of the QuorumSet that should be downloaded
    // with the statement.
    // note: the companion hash for an EXTERNALIZE statement does
    // not match the hash of the QSet, but the hash of commitQuorumSetHash
    static Hash getCompanionQuorumSetHashFromStatement(SCPStatement const& st);

    // returns the QuorumSet that should be used for a node given the
    // statement
    SCPQuorumSetPtr getQuorumSetFromStatement(SCPStatement const& st) const;

    // wraps a statement in an envelope (sign it, etc)
    SCPEnvelope createEnvelope(SCPStatement const& statement);

    // ** helper methods to stringify ballot for logging

    std::string ballotToStr(SCPBallot const& ballot) const;
    std::string ballotToStr(std::unique_ptr<SCPBallot> const& ballot) const;
    std::string envToStr(SCPEnvelope const& envelope) const;
    std::string envToStr(SCPStatement const& st) const;
};
}
