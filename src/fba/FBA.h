#ifndef __FBA__
#define __FBA__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include <map>
#include <memory>

#include "generated/FBAXDR.h"

namespace stellar
{

class Node;
class LocalNode;

class FBA
{
  public:
    /**
     * Users of the FBA library must provide an implementation of the Client
     * class. The Client methods are called by the FBA implementation to:
     *
     * 1) inform about events happening within the consensus algorithm
     *    ( `ballotPrepared`, `ballotCommitted`, `valueExternalized`)
     * 2) trigger the retrieval of data required by the FBA protocol
     *    (`retrieveQuorumSet`)
     * 3) trigger the broadcasting of FBA Envelopes to other nodes in the 
     *    network (`emitEnvelope`) 
     * 4) hand over the validation of ballots to the user of the library 
     *    (`validateBallot`)
     *    
     * The Client interface permits the abstraction of the transport layer used
     * from the actual implementation of the FBA protocol. By providing an
     * implementation of `Client` and calling `receiveQuorumSet` and
     * `receiveEnvelope` as needed, users of FBA has full control on the
     * transport layer and protocol they want to rely on.
     */
    class Client : public enable_shared_from_this<Client>
    {
        virtual void validateBallot(const uint32& slotIndex,
                                    const uint256& nodeID,
                                    const FBABallot& ballot,
                                    std::function<void(bool)> const& cb) = 0;

        virtual void ballotPrepared(const uint32& slotIndex,
                                    const FBABallot& ballot) {};
        virtual void ballotCommitted(const uint32& slotIndex,
                                     const FBABallot& ballot) {};

        virtual void valueExternalized(const uint32& slotIndex,
                                       const uint256& valueHash) = 0;

        virtual void retrieveQuorumSet(const uint256& nodeID,
                                       const uint256& qSetHash) = 0;
        virtual void emitEnvelope(const FBAEnvelope& envelope) = 0;
    };

    // Constructor
    FBA(bool validating, 
        const FBAQuorumSet& qSetLocal,
        std::shared_ptr<Client> client);

    // FBAQuorumSet/Envelope receival
    void receiveQuorumSet(const FBAQuorumSet& qSet);
    void receiveEnvelope(const FBAEnvelope& envelope);

    // Value submission
    bool attemptValue(const uint32& slotIndex,
                      const uint256& valueHash);

    // Local QuorumSet interface (can be dynamically updated)
    void setLocalQuorumSet(const FBAQuorumSet& qSet);
    const FBAQuorumSet& getLocalQuorumSet();

  private:
    bool                           mValidatingNode;
    std::shared_ptr<Client>        mClient;
    LocalNode*                     mLocalNode;
    std::map<uint256, Node*>       mKnownNodes;
}
}

#endif
