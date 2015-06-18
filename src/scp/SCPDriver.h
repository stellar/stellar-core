#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <map>
#include <set>
#include <memory>
#include <functional>
#include <chrono>

#include "generated/Stellar-SCP.h"

namespace stellar
{
typedef std::shared_ptr<SCPQuorumSet> SCPQuorumSetPtr;

class SCPDriver
{
  public:
    virtual ~SCPDriver()
    {
    }

    // Delegates the retrieval of the quorum set designated by `qSetHash` to
    // the user of SCP.
    virtual SCPQuorumSetPtr getQSet(Hash const& qSetHash) = 0;

    // Users of the SCP library should inherit from SCPDriver and implement the
    // virtual methods which are called by the SCP implementation to
    // abstract the transport layer used from the implementation of the SCP
    // protocol.

    // Delegates the emission of an SCPEnvelope to the user of SCP. Envelopes
    // should be flooded to the network.
    virtual void emitEnvelope(SCPEnvelope const& envelope) = 0;

    // methods to hand over the validation and ordering of values and ballots.

    // `validateValue` is called on each message received before any processing
    // is done. It should be used to filter out values that are not compatible
    // with the current state of that node. Unvalidated values can never
    // externalize.
    virtual bool
    validateValue(uint64 slotIndex, NodeID const& nodeID, Value const& value)
    {
        return true;
    }

    // `getValueString` is used for debugging
    // default implementation is the hash of the value
    virtual std::string getValueString(Value const& v) const;

    // `computeHash` is used by the nomination protocol to
    // randomize the order of messages between nodes.
    virtual uint64 computeHash(uint64 slotIndex, bool isPriority,
                               int32_t roundNumber, NodeID const& nodeID,
                               Value const& prev);

    // `combineCandidates` computes the composite value based off a list
    // of candidate values.
    virtual Value combineCandidates(uint64 slotIndex,
                                    std::set<Value> const& candidates) = 0;

    // `setupTimer`: requests to trigger 'cb' after timeout
    virtual void setupTimer(uint64 slotIndex, int timerID,
                            std::chrono::milliseconds timeout,
                            std::function<void()> cb) = 0;

    // Inform about events happening within the consensus algorithm.

    // `valueExternalized` is called at most once per slot when the slot
    // externalize its value.
    virtual void
    valueExternalized(uint64 slotIndex, Value const& value)
    {
    }

    // ``nominatingValue`` is called every time the local instance nominates
    // a new value.
    virtual void
    nominatingValue(uint64 slotIndex, Value const& value)
    {
    }

    // the following methods are used for monitoring of the SCP subsystem
    // most implementation don't really need to do anything with these

    // `updatedCandidateValue` is called every time a new candidate value
    // is included in the candidate set, the value passed in is
    // a composite value
    virtual void
    updatedCandidateValue(uint64 slotIndex, Value const& value)
    {
    }

    // `startedBallotProtocol` is called when the ballot protocol is started
    // (ie attempts to prepare a new ballot)
    virtual void
    startedBallotProtocol(uint64 slotIndex, SCPBallot const& ballot)
    {
    }

    // `acceptedBallotPrepared` every time a ballot is accepted as prepared
    virtual void
    acceptedBallotPrepared(uint64 slotIndex, SCPBallot const& ballot)
    {
    }

    // `confirmedBallotPrepared` every time a ballot is confirmed prepared
    virtual void
    confirmedBallotPrepared(uint64 slotIndex, SCPBallot const& ballot)
    {
    }

    // `acceptedCommit` every time a ballot is accepted commit
    virtual void
    acceptedCommit(uint64 slotIndex, SCPBallot const& ballot)
    {
    }

    // `ballotDidHearFromQuorum` is called when we received messages related to
    // the current `mBallot` from a set of node that is a transitive quorum for
    // the local node.
    virtual void
    ballotDidHearFromQuorum(uint64 slotIndex, SCPBallot const& ballot)
    {
    }

    // Hooks for subclasses to note signatures and verification pass/fail.
    virtual void
    envelopeSigned()
    {
    }

    virtual void
    envelopeVerified(bool)
    {
    }
};
}
