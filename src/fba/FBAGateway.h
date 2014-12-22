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

    class Dispatch : public enable_shared_from_this<Dispatch>
    {
        typedef std::shared_ptr<Dispatch> pointer;

        virtual bool validateBallot(const fbaxdr::uint256 nodeID,
                                    const fbaxdr::SlotBallot& ballot) = 0;

        virtual void ballotCommitted(const fbaxdr::SlotBallot& ballot) = 0;
        virtual void slotExternalized(const fbaxdr::SlotBallot& ballot) = 0;

        virtual void retrieveQuorumSet(const fbaxdr::uint256& qSetHash) = 0;
        virtual void emitEnvelope(const fbaxdr::Envelope&) = 0;
    };

    virtual void setDispatcher(const Dispatch::pointer disptacher) = 0;

    // Ballot preparation and validation
    virtual bool prepareBallot(const fbaxdr::SlotBallot& ballot) = 0;

    // QuorumSet retrieval interface
    virtual void recvQuorumSet(const fbaxdr::QuorumSet& qset) = 0;

    // Statement envelope emission and reception interface
    virtual void recvEvenlope(const fbaxdr::Envelope& envelope) = 0;

    // State interface
    virtual QuorumSet::pointer getOurQuorumSet() = 0;
    virtual Node::pointer getNode(fbaxdr::uint256& nodeID) = 0;
};
}

#endif
