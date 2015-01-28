#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include <map>
#include <memory>
#include <functional>

#include "generated/FBAXDR.h"

namespace stellar
{
class Node;
class Slot;
class LocalNode;

class FBA
{

  public:
    FBA(const uint256& validationSeed,
        const FBAQuorumSet& qSetLocal);
    ~FBA();

    // Users of the FBA library should inherit from FBA and implement the
    // following virtual methods which are called by the FBA implementation to:
    //
    // 1) hand over the validation and ordering of values and ballots.
    //    (`validateValue`, `compareValues`, `validateBallot`)
    // 2) inform about events happening within the consensus algorithm.
    //    ( `ballotDidPrepare`, `ballotDidPrepared`, `ballotDidCommit`,
    //      `ballotDidCommitted`, `valueExternalized`)
    // 3) retrieve data required by the FBA protocol.
    //    (`retrieveQuorumSet`)
    // 4) trigger the broadcast of Envelopes to other nodes in the network.
    //    (`emitEnvelope`) 
    //
    // Thes methods are designed to abstract the transport layer used from the
    // implementation of the FBA protocol.
    
    // `validateValue` is called on each message received before any processing
    // is done. It should be used to filter out values that are not compatible
    // with the current state of that node. Unvalidated values can never
    // externalize.
    virtual void validateValue(const uint64& slotIndex,
                               const uint256& nodeID,
                               const Value& value,
                               std::function<void(bool)> const& cb)
    {
        return cb(true);
    }

    // `compareValues` is used in ballot comparison. Ballots with higher values
    // cancel ones with lower values for the same ballot counter which
    // prioritize higher values.
    virtual int compareValues(const Value& v1, const Value& v2)
    {
        using xdr::operator<;

        if (v1 < v2) return -1;
        if (v2 < v1) return 1;
        return 0;
    }

    // `validateBallot` is used to validate ballots associated with PREPARE
    // messages.  Therefore unvalidated ballots may still externalize if other
    // nodes PREARED and subsequently COMMITTED such ballot. 
    virtual void validateBallot(const uint64& slotIndex,
                                const uint256& nodeID,
                                const FBABallot& ballot,
                                std::function<void(bool)> const& cb)
    {
        return cb(true);
    }

    // `ballotDidPrepare` is called each time the local node PREPARE a ballot.
    // It is always called on the internally monotically increasing `mBallot`.
    virtual void ballotDidPrepare(const uint64& slotIndex,
                                  const FBABallot& ballot) {}
    // `ballotDidPrepared` is called each time the local node PREPARED a
    // ballot. It can be called on ballots lower than `mBallot`.
    virtual void ballotDidPrepared(const uint64& slotIndex,
                                  const FBABallot& ballot) {}
    // `ballotDidCommit` is called each time the local node COMMIT a ballot.
    // It is always called on the internally monotically increasing `mBallot`.
    virtual void ballotDidCommit(const uint64& slotIndex,
                                 const FBABallot& ballot) {};
    // `ballotDidCommitted` is called each time the local node COMMITTED a
    // ballot. It is always called on the internally monotically increasing
    // `mBallot`. Once COMMITTED, a slot cannot switch to another value. That
    // does not mean that the network agress on it yet though, but if the slot
    // later externalize on this node, it will necessarily be on this value.
    virtual void ballotDidCommitted(const uint64& slotIndex,
                                    const FBABallot& ballot) {};

    // `ballotDidHearFromQuorum` is called when we received messages related to
    // the current `mBallot` from a set of node that is a transitive quorum for 
    // the local node. It should be used to start ballot expiration timer.
    virtual void ballotDidHearFromQuorum(const uint64& slotIndex,
                                         const FBABallot& ballot) {};

    // `valueExternalized` is called at most once per slot when the slot
    // externalize its value.
    virtual void valueExternalized(const uint64& slotIndex,
                                   const Value& value) {}

    // Delegates the retrieval of the quorum set designated by `qSetHash` to
    // the user of FBA.
    virtual void retrieveQuorumSet(
        const uint256& nodeID,
        const Hash& qSetHash,
        std::function<void(const FBAQuorumSet&)> const& cb) = 0;

    // Delegates the emission of an FBAEnvelope to the user of FBA. Envelopes
    // should be flooded to the network.
    virtual void emitEnvelope(const FBAEnvelope& envelope) = 0;


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
    void receiveEnvelope(const FBAEnvelope& envelope,
                         std::function<void(EnvelopeState)> const& cb = 
                         [] (EnvelopeState) { });


    // Submit a value for the FBA consensus phase
    bool prepareValue(const uint64& slotIndex,
                      const Value& value,
                      bool forceBump = false);
                       

    // Local QuorumSet interface (can be dynamically updated)
    void updateLocalQuorumSet(const FBAQuorumSet& qSet);
    const FBAQuorumSet& getLocalQuorumSet();

    // Local nodeID getter
    const uint256& getLocalNodeID();


  private:
    // Node getter
    Node* getNode(const uint256& nodeID);
    LocalNode* getLocalNode();

    // Slot getter
    Slot* getSlot(const uint64& slotIndex);

    // Envelope signature/verification
    void signEnvelope(FBAEnvelope& envelope);
    bool verifyEnvelope(const FBAEnvelope& envelope);

    LocalNode*                     mLocalNode;
    std::map<uint256, Node*>       mKnownNodes;
    std::map<uint64, Slot*>        mKnownSlots;

    friend class Slot;
    friend class Node;
};
}

