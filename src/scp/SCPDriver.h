// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "util/NonCopyable.h"
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>

#include "xdr/Stellar-SCP.h"

namespace stellar
{
class TxSetXDRFrame;
using TxSetXDRFrameConstPtr = std::shared_ptr<TxSetXDRFrame const>;

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

    // Should be called when a tx set becomes available after this wrapper was
    // created without it.
    virtual void
    setTxSet(TxSetXDRFrameConstPtr txSet)
    {
    }
};

typedef std::shared_ptr<SCPQuorumSet> SCPQuorumSetPtr;
typedef std::shared_ptr<ValueWrapper> ValueWrapperPtr;

// Called only when the local node is the leader of a round and needs to build
// its own value. May return nullptr if the application can no longer propose
// for the slot. The callback must remain valid for the lifetime of the slot.
using ProposalValueSupplier = std::function<ValueWrapperPtr()>;

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

    // Should be called when a tx set becomes available after this wrapper was
    // created without it.
    virtual void
    addTxSet(TxSetXDRFrameConstPtr txSet)
    {
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

    // `getTxSetDownloadWaitTime` returns how long the fetcher has been waiting
    // for the transaction set identified by @p hash. Returns nullopt if the
    // transaction set is not being fetched. May throw if `hash` cannot be
    // converted to a `StellarValue`.
    virtual std::optional<std::chrono::milliseconds>
    getTxSetDownloadWaitTime(Value const& hash) const = 0;

    // Returns how long the ballot protocol should wait before replacing a
    // value whose transaction set has not finished downloading.
    virtual std::chrono::milliseconds getTxSetDownloadTimeout() const = 0;

    // Returns whether or not the envelope `env` is ready to be processed by
    // SCP.
    virtual bool isEnvelopeReady(SCPEnvelope const& env) const = 0;

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
    // externalize. Neither can values that are only structurally valid (they
    // reference a missing or invalid transaction set.)
    // If the value cannot be validated (node is missing some context due to the
    // value belonging to a ledger other than LCL+1) but passes the validity
    // checks, kMaybeValidNotCurrentValue can be returned. This will cause the
    // current slot to be marked as a non validating slot: the local node will
    // abstain from emitting its position.
    // kMaybeValidNotCurrentValue should only be returned if the value is for a
    // ledger other than current ledger (that is, not LCL+1). If a value cannot
    // be validated due to parallel downloading (e.g. it's for LCL+1 but the
    // node is still downloading the tx set), then kStructurallyValidValue
    // should be used.
    // validation is *more* restrictive when `proposal` is set: that mode is
    // used before the local node adopts (votes for) a round leader's value,
    // and for the leader's own value. It is where the application enforces
    // what it is willing to propose (e.g. only upgrades it has armed).
    // NB: validation levels are ordered
    enum ValidationLevel
    {
        // Value is known to be invalid
        kInvalidValue = 0,
        // Value may be valid, but it is for some ledger other than LCL+1 and so
        // cannot be fully validated.
        kMaybeValidNotCurrentValue = 1,
        // Value is for LCL+1 and is structurally valid (close time is valid,
        // etc), but the transaction set it references is either missing or
        // invalid.
        kStructurallyValidValue = 2,
        // Value is for LCL+1 and is known to be fully valid
        kFullyValidatedValue = 3
    };
    virtual ValidationLevel
    validateValue(uint64 slotIndex, Value const& value, bool proposal) const
    {
        return kMaybeValidNotCurrentValue;
    }

    // Helper function to craft an empty-tx-set value from a Value.
    virtual Value makeEmptyTxSetValueFromValue(Value const& v) const = 0;

    // `isEmptyTxSetValue` checks if a value is an empty-tx-set value.
    virtual bool isEmptyTxSetValue(Value const& v) const = 0;

    // Whether or not to allow downloading of transaction sets in parallel with
    // SCP
    virtual bool isParallelTxSetDownloadEnabled() const = 0;

    // The protocol version allows the use of STELLAR_VALUE_EMPTY_TX_SET
    virtual bool protocolAllowsEmptyTxSetValues() const = 0;

    // `getValueString` is used for debugging
    // default implementation is the hash of the value
    virtual std::string getValueString(Value const& v) const;

    // `toStrKey` returns StrKey encoded string representation
    virtual std::string toStrKey(NodeID const& pk, bool fullKey = true) const;

    // `toShortString` converts to the common name of a key if found
    virtual std::string toShortString(NodeID const& pk) const;

    // `getHashOf` computes the hash for the given vector of byte vector
    virtual Hash
    getHashOf(std::vector<xdr::opaque_vec<>> const& vals) const = 0;

    // `getRoundLeader` returns the leader of round `round` (1-based, a round
    // is a ballot counter) for `slotIndex`. Every node that takes part in
    // consensus on the slot must compute the same answer. Returns nullopt when
    // the schedule is not known for that slot (e.g. the slot is not the next
    // one to close), in which case the local node neither proposes nor adopts.
    virtual std::optional<NodeID> getRoundLeader(uint64 slotIndex,
                                                 uint32 round) = 0;

    // `isProposalPending` returns true if the application holds a statement
    // of `leader` for `slotIndex` that it has not handed to SCP yet (e.g. it
    // is still fetching the transaction set of its value). SCP then gives a
    // round leader one more timeout before moving to the next round.
    virtual bool
    isProposalPending(uint64 slotIndex, NodeID const& leader) const
    {
        return false;
    }

    // `setupTimer`: requests to trigger 'cb' after timeout
    // if cb is nullptr, the timer is cancelled
    virtual void setupTimer(uint64 slotIndex, int timerID,
                            std::chrono::milliseconds timeout,
                            std::function<void()> cb) = 0;
    virtual void stopTimer(uint64 slotIndex, int timerID) = 0;

    // `computeTimeout` computes a timeout given a round number
    // it should be sufficiently large such that nodes in a
    // quorum can exchange 4 messages
    virtual std::chrono::milliseconds computeTimeout(uint32 roundNumber) = 0;

    // Inform about events happening within the consensus algorithm.

    // `valueExternalized` is called at most once per slot when the slot
    // externalize its value.
    virtual void
    valueExternalized(uint64 slotIndex, Value const& value)
    {
    }

    virtual void
    noteEmptyTxSetValueReplaced(uint64)
    {
    }

    // `reproposeValue` is called when the local node, as the leader of
    // `round`, is about to put forward `value`, the value it already votes
    // for, rather than building a new one. It may return an equivalent value
    // to propose instead (e.g. the same content under the local node's own
    // signature), or nullptr to propose `value` as it is.
    virtual ValueWrapperPtr
    reproposeValue(uint64 slotIndex, uint32 round, Value const& value)
    {
        return nullptr;
    }

    // the following methods are used for monitoring of the SCP subsystem
    // most implementation don't really need to do anything with these

    // `proposedValue` is called when the local node, as the leader of
    // `round`, puts `value` forward.
    virtual void
    proposedValue(uint64 slotIndex, uint32 round, Value const& value)
    {
    }

    // `enteredRound` is called when the local node moves to `round` for
    // `slotIndex`. `timedOut` is true when a timer expiry caused the move.
    virtual void
    enteredRound(uint64 slotIndex, uint32 round, bool timedOut)
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

    // Called when balloting becomes blocked waiting for a txset download
    virtual void
    recordBallotBlockedOnTxSet(uint64 slotIndex, Value const& value)
    {
    }

    // Called when balloting is unblocked (setting mCommit) to measure and
    // record how long we were blocked waiting for the txset
    virtual void
    measureAndRecordBallotBlockedOnTxSet(uint64 slotIndex, Value const& value)
    {
    }
};
}
