#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <map>
#include <set>
#include <memory>
#include <functional>
#include <chrono>

#include "crypto/SecretKey.h"

#include "generated/SCPXDR.h"

namespace stellar
{
class Node;
class Slot;
class LocalNode;
typedef std::shared_ptr<SCPQuorumSet> SCPQuorumSetPtr;

class SCP
{

  public:
    SCP(SecretKey const& secretKey, SCPQuorumSet const& qSetLocal);
    virtual ~SCP()
    {
    }

    // Users of the SCP library should inherit from SCP and implement the
    // following virtual methods which are called by the SCP implementation to:
    //
    // 1) hand over the validation and ordering of values and ballots.
    //    (`validateValue`, `validateBallot`)
    // 2) inform about events happening within the consensus algorithm.
    //    ( `ballotDidPrepare`, `ballotDidPrepared`, `ballotDidCommit`,
    //      `ballotDidCommitted`, `valueExternalized`)
    // 3) retrieve data required by the SCP protocol.
    //    (`retrieveQuorumSet`)
    // 4) trigger the broadcast of Envelopes to other nodes in the network.
    //    (`emitEnvelope`)
    //
    // These methods are designed to abstract the transport layer used from the
    // implementation of the SCP protocol.

    // `validateValue` is called on each message received before any processing
    // is done. It should be used to filter out values that are not compatible
    // with the current state of that node. Unvalidated values can never
    // externalize.
    virtual bool
    validateValue(uint64 slotIndex, uint256 const& nodeID, Value const& value)
    {
        return true;
    }

    // `compareValues` is used in ballot comparison. Ballots with higher values
    // cancel ones with lower values for the same ballot counter which
    // prioritize higher values. It is acceptable to parametrize the value
    // ordering by slotIndex and ballot counter. That's why they are provided
    // as argument to `compareValues`.
    virtual int
    compareValues(uint64 slotIndex, uint32 const& ballotCounter,
                  Value const& v1, Value const& v2)
    {
        using xdr::operator<;

        if (v1 < v2)
            return -1;
        if (v2 < v1)
            return 1;
        return 0;
    }

    // `getValueString` is used for debugging
    // default implementation is the hash of the value
    virtual std::string getValueString(Value const& v) const;

    // `validateBallot` is used to validate ballots associated with PREPARING
    // messages.  Therefore unvalidated ballots may still externalize if other
    // nodes PREARED and subsequently COMMITTED such ballot.
    virtual bool
    validateBallot(uint64 slotIndex, uint256 const& nodeID,
                   SCPBallot const& ballot)
    {
        return true;
    }

    // `computeHash` is used by the nomination protocol to
    // randomize the order of messages between nodes.
    virtual uint64 computeHash(uint64 slotIndex, bool isPriority, int32 roundNumber,
                               uint256 const& nodeID);

    // `ballotDidPrepare` is called each time the local node PREPARING a ballot.
    // It is always called on the internally monotonically increasing `mBallot`.
    virtual void
    ballotDidPrepare(uint64 slotIndex, SCPBallot const& ballot)
    {
    }
    // `ballotDidPrepared` is called each time the local node PREPARED a
    // ballot. It can be called on ballots lower than `mBallot`.
    virtual void
    ballotDidPrepared(uint64 slotIndex, SCPBallot const& ballot)
    {
    }
    // `ballotDidCommit` is called each time the local node COMMITTING a ballot.
    // It is always called on the internally monotonically increasing `mBallot`.
    virtual void
    ballotDidCommit(uint64 slotIndex, SCPBallot const& ballot)
    {
    }
    // `ballotDidCommitted` is called each time the local node COMMITTED a
    // ballot. It is always called on the internally monotonically increasing
    // `mBallot`. Once COMMITTED, a slot cannot switch to another value. That
    // does not mean that the network agrees on it yet though, but if the slot
    // later externalize on this node, it will necessarily be on this value.
    virtual void
    ballotDidCommitted(uint64 slotIndex, SCPBallot const& ballot)
    {
    }

    // `ballotDidHearFromQuorum` is called when we received messages related to
    // the current `mBallot` from a set of node that is a transitive quorum for
    // the local node. It should be used to start ballot expiration timer.
    virtual void
    ballotDidHearFromQuorum(uint64 slotIndex, SCPBallot const& ballot)
    {
    }

    // `ballotGotBumped` is called every time the local ballot is updated
    // timeout is the duration that the local instance should wait for before
    // calling `abandonBallot`
    virtual void
    ballotGotBumped(uint64 slotIndex, SCPBallot const& ballot,
                    std::chrono::milliseconds timeout)
    {
    }

    // `valueExternalized` is called at most once per slot when the slot
    // externalize its value.
    virtual void
    valueExternalized(uint64 slotIndex, Value const& value)
    {
    }

    // ``nominatingValue`` is called every time the local instance nominates
    // a new value.
    // timeout is the duration for which the local instance should wait before
    // calling 'nominate' with the 'timedout' flag set to true.
    virtual void
    nominatingValue(uint64 slotIndex, Value const& value,
                    std::chrono::milliseconds timeout)
    {
    }

    // `combineCandidates` computes the composite value based off a list
    // of candidate values.
    virtual Value combineCandidates(uint64 slotIndex,
                                    std::set<Value> const& candidates) = 0;

    // `nodeTouched` is call whenever a node is used within the SCP consensus
    // protocol. It lets implementor of SCP evict nodes that haven't been
    // touched for a long time (because it died or the quorum structure was
    // updated).
    virtual void
    nodeTouched(uint256 const& nodeID)
    {
    }

    // Delegates the retrieval of the quorum set designated by `qSetHash` to
    // the user of SCP.
    virtual SCPQuorumSetPtr getQSet(Hash const& qSetHash) = 0;

    // Delegates the emission of an SCPEnvelope to the user of SCP. Envelopes
    // should be flooded to the network.
    virtual void emitEnvelope(SCPEnvelope const& envelope) = 0;

    enum EnvelopeState
    {
        INVALID, // the envelope is considered invalid
        VALID    // the envelope is valid
    };
    // If evidences are missing, a retransmission should take place for that
    // slot. see `procudeSlotEvidence` to implement such retransmission
    // mechanism on the other side of the wire.
    EnvelopeState receiveEnvelope(SCPEnvelope const& envelope);

    // request to trigger a 'bumpState'
    // returns the value returned by 'bumpState'
    bool abandonBallot(uint64 slotIndex);

    // Submit a value for the SCP consensus phase
    bool nominate(uint64 slotIndex, Value const& value, bool timedout);

    // Local QuorumSet interface (can be dynamically updated)
    void updateLocalQuorumSet(SCPQuorumSet const& qSet);
    SCPQuorumSet const& getLocalQuorumSet();

    // Local nodeID getter
    uint256 const& getLocalNodeID();

    // returns the local node descriptor
    std::shared_ptr<LocalNode> getLocalNode();

    // Envelope signature/verification
    void signEnvelope(SCPEnvelope& envelope);
    bool verifyEnvelope(SCPEnvelope const& envelope);

    // Node getters
    std::shared_ptr<Node> getNode(uint256 const& nodeID);

  protected:
    std::shared_ptr<LocalNode> mLocalNode;
    std::map<uint256, std::shared_ptr<Node>> mKnownNodes;
    std::map<uint64, std::shared_ptr<Slot>> mKnownSlots;

    // Purges all data relative to that node. Can be called at any time on any
    // node. If the node is subsequently needed, it will be recreated and its
    // quorumSet retrieved again. This method has no effect if called on the
    // local nodeID.
    void purgeNode(uint256 const& nodeID);

    // Purges all data relative to all the slots whose slotIndex is smaller
    // than the specified `maxSlotIndex`.
    void purgeSlots(uint64 maxSlotIndex);

    // Retrieves the local secret key as specified at construction
    SecretKey const& getSecretKey();

    // Tests whether a set of nodes is v-blocking for our local node. This can
    // be used in ballot validation decisions.
    bool isVBlocking(std::vector<uint256> const& nodes);

    // Hooks for subclasses to note signatures and verification pass/fail.
    virtual void
    envelopeSigned()
    {
    }
    virtual void
    envelopeVerified(bool)
    {
    }

    // Helpers for monitoring and reporting the internal memory-usage of the SCP
    // protocol to system metric reporters.
    size_t getKnownNodesCount() const;
    size_t getKnownSlotsCount() const;
    size_t getCumulativeStatemtCount() const;

  private:
    // Slot getter
    std::shared_ptr<Slot> getSlot(uint64 slotIndex);
};
}
