#ifndef __FBA__
#define __FBA__

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

//using xdr::operator<;


class FBA
{
  public:
    /**
     * Users of the FBA library must provide an implementation of the
     * FBA::Client class. The Client methods are called by the FBA
     * implementation to:
     *
     * 1) hand over the validation of ballots to the user of the library 
     *    (`validateBallot`)
     * 2) inform about events happening within the consensus algorithm
     *    ( `ballotDidPrepare`, `ballotDidCommit`, `valueCancelled`, 
     *      `valueExternalized`)
     * 3) trigger the retrieval of data required by the FBA protocol
     *    (`retrieveQuorumSet`)
     * 4) trigger the broadcasting of FBA Envelopes to other nodes in the 
     *    network (`emitEnvelope`) 
     * 5) hint rentransmissions when it can be detected by the protocol
     *    (`retransmissionHinted`)
     *    
     * The FBA::Client interface not only informs the outside world about the
     * progress made by FBA but it also permits the abstraction of the
     * transport layer used from the actual implementation of the FBA protocol. 
     */
    class Client
    {
      public:
        virtual void validateBallot(const uint64& slotIndex,
                                    const uint256& nodeID,
                                    const FBABallot& ballot,
                                    std::function<void(bool)> const& cb) = 0;
        virtual int compareValues(const Hash& v1,
                                  const Hash& v2);

        virtual void ballotDidPrepare(const uint64& slotIndex,
                                      const FBABallot& ballot) {}
        virtual void ballotDidCommit(const uint64& slotIndex,
                                     const FBABallot& ballot) {};

        virtual void valueCancelled(const uint64& slotIndex,
                                    const Hash& valueHash) {}
        virtual void valueExternalized(const uint64& slotIndex,
                                       const Hash& valueHash) {}

        virtual void retrieveQuorumSet(const uint256& nodeID,
                                       const Hash& qSetHash) = 0;
        virtual void emitEnvelope(const FBAEnvelope& envelope) = 0;
        
        virtual void retransmissionHinted(const uint64& slotIndex,
                                          const uint256& nodeID) {};
    };

    // The constructor is passed an FBA::Client object but does not own it. The
    // FBA::Client must outlive the FBA object itself.
    FBA(const uint256& validationSeed,
        const FBAQuorumSet& qSetLocal,
        Client* client);
    ~FBA();

    // FBAQuorumSet/Envelope receival
    void receiveQuorumSet(const uint256& nodeID,
                          const FBAQuorumSet& qSet);
    void receiveEnvelope(const FBAEnvelope& envelope);

    // Value submission
    bool attemptValue(const uint64& slotIndex,
                      const Hash& valueHash);

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
    // FBA::Client getter
    Client* getClient();

    Client*                        mClient;
    LocalNode*                     mLocalNode;
    std::map<uint256, Node*>       mKnownNodes;
    std::map<uint64, Slot*>        mKnownSlots;

    friend class Slot;
    friend class Node;
};
}

#endif
