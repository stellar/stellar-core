#ifndef __FBAGATEWAY__
#define __FBAGATEWAY__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "fba/FBA.h"

namespace stellar
{
/**
 * The public interface to the FBA module
 */
class FBAGateway
{
  public:
    virtual void setValidating(bool validating) = 0;

    /**
     * Users of the FBA library must provide an implementation of the Dispatch 
     * class. The Dispatch methods are called by the FBA implementation to:
     * 1) inform about events happening within the consensus algorithm
     *    (`ballotCommitted`, `slotExternalized`)
     * 2) trigger the retrieval of data required by the FBA protocol 
     *    (`retrieveQuorumSet`)
     * 3) trigger the broadcasting of FBA Envelopes to other nodes in the 
     *    network (`emitEnvelope`) 
     * 4) hand over the validation of ballots to the user of the library 
     *    (`validateBallot`)
     * The Dispatch interface permits the abstraction of the transport layer 
     * used from the actual implementation of the FBA protocol. By providing an 
     * implementation of `Dispatch` and calling `receiveQuorumSet` and 
     * `receiveEnvelope` as needed, the user of FBA has full control on the 
     * transport she relies on.
     */
    class Dispatch : public enable_shared_from_this<Dispatch>
    {
        typedef std::shared_ptr<Dispatch> pointer;

        virtual bool validateBallot(const uint256 nodeID,
                                    const SlotBallot& ballot) = 0;

        virtual void ballotCommitted(const SlotBallot& ballot) = 0;
        virtual void slotExternalized(const SlotBallot& ballot) = 0;

        virtual void retrieveQuorumSet(const uint256& qSetHash) = 0;
        virtual void emitEnvelope(const Envelope&) = 0;
    };

    // Dispatch interface and QuorumSet/Envelope receival
    virtual void setDispatcher(const Dispatch::pointer disptacher) = 0;
    virtual void receiveQuorumSet(const QuorumSet& qset) = 0;
    virtual void receiveEnvelope(const Envelope& envelope) = 0;

    // Ballot preparation
    virtual bool prepareBallot(const SlotBallot& ballot) = 0;

    // Local QuorumSet interface
    virtual void setLocalQuorumSet(const QuorumSet& qset) = 0;
    virtual const QuorumSet& getLocalQuorumSet() = 0;
};
}

#endif
