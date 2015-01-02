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

typedef std::shared_ptr<FBABallot> FBABallotPtr;
typedef std::shared_ptr<FBASlotBallot> FBASlotBallotPtr;
typedef std::shared_ptr<FBAStatement> FBAStatementPtr;
typedef std::shared_ptr<FBAQuorumSet> FBAQuorumSetPtr;

class FBA
{
  public:
    /**
     * Users of the FBA library must provide an implementation of the Dispatch
     * class. The Dispatch methods are called by the FBA implementation to:
     *
     * 1) inform about events happening within the consensus algorithm
     *    (`ballotCommitted`, `slotExternalized`)
     * 2) trigger the retrieval of data required by the FBA protocol
     *    (`retrieveQuorumSet`)
     * 3) trigger the broadcasting of FBA Envelopes to other nodes in the 
     *    network (`emitEnvelope`) 
     * 4) hand over the validation of ballots to the user of the library 
     *    (`validateBallot`)
     *    
     * The Dispatch interface permits the abstraction of the transport layer
     * used from the actual implementation of the FBA protocol. By providing an
     * implementation of `Dispatch` and calling `receiveQuorumSet` and
     * `receiveEnvelope` as needed, users of FBA has full control on the
     * transport layer and protocol they want to rely on.
     */
    class Dispatch : public enable_shared_from_this<Dispatch>
    {
        virtual bool validateBallot(const uint256& nodeID,
                                    const FBASlotBallot& ballot) = 0;

        virtual void ballotCommitted(const FBASlotBallot& ballot) = 0;
        virtual void slotExternalized(const FBASlotBallot& ballot) = 0;

        virtual void retrieveQuorumSet(const uint256& nodeID,
                                       const uint256& qSetHash) = 0;
        virtual void emitEnvelope(const FBAEnvelope& envelope) = 0;
    };

    typedef std::shared_ptr<Dispatch> DispatchPtr;

    // Constructor
    FBA(bool validating, 
        const FBAQuorumSet& qSetLocal,
        DispatchPtr dispatch);

    // FBAQuorumSet/Envelope receival
    void receiveQuorumSet(const FBAQuorumSet& qSet);
    void receiveEnvelope(const FBAEnvelope& envelope);

    // Ballot preparation
    bool prepareBallot(const FBASlotBallot& ballot);

    // Local QuorumSet interface (can be dynamically updated)
    void setLocalQuorumSet(const FBAQuorumSet& qset);
    const FBAQuorumSet& getLocalQuorumSet();

  private:
    bool                                      mValidatingNode;
    DispatchPtr                               mDispatch;
    std::shared_ptr<LocalNode>                mLocalNode;
    std::map<uint256, std::shared_ptr<Node>>  mKnownNodes;
}
}

#endif
