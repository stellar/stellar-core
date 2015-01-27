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
    // 1) hand over the validation of values and ballots.
    //    (`validateValue`, `validateBallot`)
    // 2) inform about events happening within the consensus algorithm.
    //    ( `ballotDidPrepare`, `ballotDidPrepared`, `ballotDidCommit`,
    //      `ballotDidAbort`, `valueExternalized`)
    // 3) retrieve data required by the FBA protocol.
    //    (`retrieveQuorumSet`)
    // 4) trigger the broadcast of Envelopes to other nodes in the network.
    //    (`emitEnvelope`) 
    //
    // Thes methods are designed to abstract the transport layer used from the
    // implementation of the FBA protocol
    
    virtual void validateValue(const uint64& slotIndex,
                               const uint256& nodeID,
                               const Value& value,
                               std::function<void(bool)> const& cb)
    {
        return cb(true);
    }

    virtual int compareValues(const Value& v1, const Value& v2)
    {
        using xdr::operator<;

        if (v1 < v2) return -1;
        if (v2 < v1) return 1;
        return 0;
    }

    virtual void validateBallot(const uint64& slotIndex,
                                const uint256& nodeID,
                                const FBABallot& ballot,
                                std::function<void(bool)> const& cb)
    {
        return cb(true);
    }


    virtual void ballotDidPrepare(const uint64& slotIndex,
                                  const FBABallot& ballot) {}
    virtual void ballotDidPrepared(const uint64& slotIndex,
                                  const FBABallot& ballot) {}
    virtual void ballotDidCommit(const uint64& slotIndex,
                                 const FBABallot& ballot) {};
    virtual void ballotDidAbort(const uint64& slotIndex,
                                const FBABallot& blalot) {};

    virtual void valueExternalized(const uint64& slotIndex,
                                   const Value& value) {}

    virtual void retrieveQuorumSet(
        const uint256& nodeID,
        const Hash& qSetHash,
        std::function<void(const FBAQuorumSet&)> const& cb) = 0;

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

  protected:
    // Retrieves the highest prepared value for the specified slot.
    // TODO(spolu) highestPreparedValue
    // Value highestPreparedValue(const uint64& slotIndex);


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

