#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/NonCopyable.h"
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <set>

#include "xdr/Stellar-SCP.h"

namespace stellar
{
class ValueWrapper : public NonMovableOrCopyable
{
    Value const mValue;

  public:
    explicit ValueWrapper(Value const& value);
    virtual ~ValueWrapper();

    Value const&
    getValue() const
    {
        return mValue;
    }
};

typedef std::shared_ptr<SCPQuorumSet> SCPQuorumSetPtr;
typedef std::shared_ptr<ValueWrapper> ValueWrapperPtr;

class WrappedValuePtrComparator
{
  public:
    bool operator()(ValueWrapperPtr const& l, ValueWrapperPtr const& r) const;
};

typedef std::set<ValueWrapperPtr, WrappedValuePtrComparator> ValueWrapperPtrSet;

class SCPEnvelopeWrapper : public NonMovableOrCopyable
{
    SCPEnvelope const mEnvelope;

  public:
    explicit SCPEnvelopeWrapper(SCPEnvelope const& e);
    virtual ~SCPEnvelopeWrapper();

    SCPEnvelope const&
    getEnvelope() const
    {
        return mEnvelope;
    }
    SCPStatement const&
    getStatement() const
    {
        return mEnvelope.statement;
    }
};

typedef std::shared_ptr<SCPEnvelopeWrapper> SCPEnvelopeWrapperPtr;

class SCPDriver
{
  public:
    virtual ~SCPDriver()
    {
    }

    // Envelope signature
    virtual void signEnvelope(SCPEnvelope& envelope) = 0;

    // SCPEnvelopeWrapper factory
    virtual SCPEnvelopeWrapperPtr wrapEnvelope(SCPEnvelope const& envelope);

    // ValueWrapperPtr factory
    virtual ValueWrapperPtr wrapValue(Value const& value);

    // Retrieves a quorum set from its hash
    //
    // All SCP statement (see `SCPNomination` and `SCPStatement`) include
    // a quorum set hash.
    // SCP does not define how quorum sets are exchanged between nodes,
    // hence their retrieval is delegated to the user of SCP.
    // The return value is not cached by SCP, as quorum sets are transient.
    //
    // `nullptr` is a valid return value which cause the statement to be
    // considered invalid.
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
    // with the current state of that node. Invalid values can never
    // externalize.
    // If the value cannot be validated (node is missing some context) but
    // passes
    // the validity checks, kMaybeValidValue can be returned. This will cause
    // the current slot to be marked as a non validating slot: the local node
    // will abstain from emiting its position.
    // validation can be *more* restrictive during nomination as needed
    // NB: validation levels are ordered
    enum ValidationLevel
    {
        kInvalidValue = 0,       // value is invalid for sure
        kMaybeValidValue = 1,    // value may be valid
        kFullyValidatedValue = 2 // value is valid for sure
    };
    virtual ValidationLevel
    validateValue(uint64 slotIndex, Value const& value, bool nomination)
    {
        return kMaybeValidValue;
    }

    // `extractValidValue` transforms the value, if possible to a different
    // value that the local node would agree to (fully validated).
    // This is used during nomination when encountering an invalid value (ie
    // validateValue did not return `kFullyValidatedValue` for this value).
    // returning nullptr means no valid value could be extracted
    virtual ValueWrapperPtr
    extractValidValue(uint64 slotIndex, Value const& value)
    {
        return nullptr;
    }

    // `getValueString` is used for debugging
    // default implementation is the hash of the value
    virtual std::string getValueString(Value const& v) const;

    // `toStrKey` returns StrKey encoded string representation
    virtual std::string toStrKey(PublicKey const& pk,
                                 bool fullKey = true) const;

    // `toShortString` converts to the common name of a key if found
    virtual std::string toShortString(PublicKey const& pk) const;

    // `getHashOf` computes the hash for the given vector of byte vector
    virtual Hash
    getHashOf(std::vector<xdr::opaque_vec<>> const& vals) const = 0;

    // `computeHashNode` is used by the nomination protocol to
    // randomize the order of messages between nodes.
    virtual uint64 computeHashNode(uint64 slotIndex, Value const& prev,
                                   bool isPriority, int32_t roundNumber,
                                   NodeID const& nodeID);

    // `computeValueHash` is used by the nomination protocol to
    // randomize the relative order between values.
    virtual uint64 computeValueHash(uint64 slotIndex, Value const& prev,
                                    int32_t roundNumber, Value const& value);

    // `combineCandidates` computes the composite value based off a list
    // of candidate values.
    virtual ValueWrapperPtr
    combineCandidates(uint64 slotIndex,
                      ValueWrapperPtrSet const& candidates) = 0;

    // `setupTimer`: requests to trigger 'cb' after timeout
    // if cb is nullptr, the timer is cancelled
    virtual void setupTimer(uint64 slotIndex, int timerID,
                            std::chrono::milliseconds timeout,
                            std::function<void()> cb) = 0;

    // `computeTimeout` computes a timeout given a round number
    // it should be sufficiently large such that nodes in a
    // quorum can exchange 4 messages
    virtual std::chrono::milliseconds computeTimeout(uint32 roundNumber);

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

  private:
    uint64
    hashHelper(uint64 slotIndex, Value const& prev,
               std::function<void(std::vector<xdr::opaque_vec<>>&)> extra);
};
}
