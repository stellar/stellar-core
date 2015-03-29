#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include <map>
#include <memory>
#include <functional>

#include "crypto/SecretKey.h"

#include "generated/SCPXDR.h"

namespace stellar
{
class Node;
class Slot;
class LocalNode;

class SCP
{

  public:
    SCP(const SecretKey& secretKey, const SCPQuorumSet& qSetLocal);
    virtual ~SCP() {}

    // Users of the SCP library should inherit from SCP and implement the
    // following virtual methods which are called by the SCP implementation to:
    //
    // 1) hand over the validation and ordering of values and ballots.
    //    (`validateValue`, `compareValues`, `validateBallot`)
    // 2) inform about events happening within the consensus algorithm.
    //    ( `ballotDidPrepare`, `ballotDidPrepared`, `ballotDidCommit`,
    //      `ballotDidCommitted`, `valueExternalized`)
    // 3) retrieve data required by the SCP protocol.
    //    (`retrieveQuorumSet`)
    // 4) trigger the broadcast of Envelopes to other nodes in the network.
    //    (`emitEnvelope`)
    //
    // Thes methods are designed to abstract the transport layer used from the
    // implementation of the SCP protocol.

    // `validateValue` is called on each message received before any processing
    // is done. It should be used to filter out values that are not compatible
    // with the current state of that node. Unvalidated values can never
    // externalize.
    virtual void
    validateValue(const uint64& slotIndex, const uint256& nodeID,
                  const Value& value, std::function<void(bool)> const& cb)
    {
        return cb(true);
    }

    // `compareValues` is used in ballot comparison. Ballots with higher values
    // cancel ones with lower values for the same ballot counter which
    // prioritize higher values. It is acceptable to parametrize the value
    // ordering by slotIndex and ballot counter. That's why they are provided
    // as argument to `compareValues`.
    virtual int
    compareValues(const uint64& slotIndex, const uint32& ballotCounter,
                  const Value& v1, const Value& v2)
    {
        using xdr::operator<;

        if (v1 < v2)
            return -1;
        if (v2 < v1)
            return 1;
        return 0;
    }

    // `validateBallot` is used to validate ballots associated with PREPARING
    // messages.  Therefore unvalidated ballots may still externalize if other
    // nodes PREARED and subsequently COMMITTED such ballot.
    virtual void
    validateBallot(const uint64& slotIndex, const uint256& nodeID,
                   const SCPBallot& ballot, std::function<void(bool)> const& cb)
    {
        return cb(true);
    }

    // `ballotDidPrepare` is called each time the local node PREPARING a ballot.
    // It is always called on the internally monotically increasing `mBallot`.
    virtual void
    ballotDidPrepare(const uint64& slotIndex, const SCPBallot& ballot)
    {
    }
    // `ballotDidPrepared` is called each time the local node PREPARED a
    // ballot. It can be called on ballots lower than `mBallot`.
    virtual void
    ballotDidPrepared(const uint64& slotIndex, const SCPBallot& ballot)
    {
    }
    // `ballotDidCommit` is called each time the local node COMMITTING a ballot.
    // It is always called on the internally monotically increasing `mBallot`.
    virtual void
    ballotDidCommit(const uint64& slotIndex, const SCPBallot& ballot)
    {
    }
    // `ballotDidCommitted` is called each time the local node COMMITTED a
    // ballot. It is always called on the internally monotically increasing
    // `mBallot`. Once COMMITTED, a slot cannot switch to another value. That
    // does not mean that the network agress on it yet though, but if the slot
    // later externalize on this node, it will necessarily be on this value.
    virtual void
    ballotDidCommitted(const uint64& slotIndex, const SCPBallot& ballot)
    {
    }

    // `ballotDidHearFromQuorum` is called when we received messages related to
    // the current `mBallot` from a set of node that is a transitive quorum for
    // the local node. It should be used to start ballot expiration timer.
    virtual void
    ballotDidHearFromQuorum(const uint64& slotIndex, const SCPBallot& ballot)
    {
    }

    // `valueExternalized` is called at most once per slot when the slot
    // externalize its value.
    virtual void
    valueExternalized(const uint64& slotIndex, const Value& value)
    {
    }

    // `nodeTouched` is call whenever a node is used within the SCP consensus
    // protocol. It lets implementor of SCP evict nodes that haven't been
    // touched for a long time (because it died or the quorum structure was
    // updated).
    virtual void
    nodeTouched(const uint256& nodeID)
    {
    }

    // Delegates the retrieval of the quorum set designated by `qSetHash` to
    // the user of SCP.
    virtual void
    retrieveQuorumSet(const uint256& nodeID, const Hash& qSetHash,
                      std::function<void(const SCPQuorumSet&)> const& cb) = 0;

    // Delegates the emission of an SCPEnvelope to the user of SCP. Envelopes
    // should be flooded to the network.
    virtual void emitEnvelope(const SCPEnvelope& envelope) = 0;

    // Receives an envelope. `cb` asynchronously returns with a status for the
    // envelope:
    enum EnvelopeState
    {
        INVALID,            // the envelope is considered invalid
        STATEMENTS_MISSING, // the envelope is valid but we miss statements
        VALID               // the envelope is valid
    };
    // If evidences are missing, a retransmisison should take place for that
    // slot. see `procudeSlotEvidence` to implement such retransmission
    // mechanism on the other side of the wire.
    void receiveEnvelope(const SCPEnvelope& envelope,
                         std::function<void(EnvelopeState)> const& cb =
                             [](EnvelopeState)
                         {
    });

    // Submit a value for the SCP consensus phase
    bool prepareValue(const uint64& slotIndex, const Value& value,
                      bool forceBump = false);

    // Local QuorumSet interface (can be dynamically updated)
    void updateLocalQuorumSet(const SCPQuorumSet& qSet);
    const SCPQuorumSet& getLocalQuorumSet();

    // Local nodeID getter
    const uint256& getLocalNodeID();

  protected:
      std::shared_ptr<LocalNode> mLocalNode;
      std::map<uint256, std::shared_ptr<Node>> mKnownNodes;
      std::map<uint64, std::shared_ptr<Slot>> mKnownSlots;

    // Purges all data relative to that node. Can be called at any time on any
    // node. If the node is subsequently needed, it will be recreated and its
    // quorumSet retrieved again. This method has no effect if called on the
    // local nodeID.
    void purgeNode(const uint256& nodeID);

    // Purges all data relative to all the slots whose slotIndex is smaller
    // than the specified `maxSlotIndex`.
    void purgeSlots(const uint64& maxSlotIndex);

    // Retrieves the local secret key as specified at construction
    const SecretKey& getSecretKey();

    // Tests wether a set of nodes is v-blocking for our local node. This can
    // be used in ballot validation decisions.
    bool isVBlocking(const std::vector<uint256>& nodes);

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
    size_t getCumulativeCachedQuorumSetCount() const;

  private:
    // Node getters
    std::shared_ptr<Node> getNode(const uint256& nodeID);
    std::shared_ptr<LocalNode> getLocalNode();

    // Slot getter
    std::shared_ptr<Slot> getSlot(const uint64& slotIndex);

    // Envelope signature/verification
    void signEnvelope(SCPEnvelope& envelope);
    bool verifyEnvelope(const SCPEnvelope& envelope);

    friend class Slot;
    friend class Node;
};
}
