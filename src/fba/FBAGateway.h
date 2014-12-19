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

    // Triggers a new round
    virtual void startNewRound(const fbaxdr::SlotBallot& ballot);

    // QuorumSet retrieval interface
    virtual void onQuorumSetNeeded(
        std::function<void(fbaxdr::uint256 const& qSetHash) const& func) = 0;
    virtual void recvQuorumSet(const fbaxdr::QuorumSet& qset) = 0;

    // Statement envelope emission and reception interface
    virtual void onEnvelopeEmitted(
        std::function<void(const fbaxdr::Envelope&)> const& func) = 0;
    virtual void recvEvenlope(const fbaxdr::Envelope& envelope) = 0;

    // Externalization interface
    virtual void onSlotExternalized(
        std::function<void(const fbaxdr::SlotBallot&)> const& func) = 0;

    // State interface
    virtual QuorumSet::pointer getOurQuorumSet() = 0;
    virtual Node::pointer getNode(fbaxdr::uint256& nodeID) = 0;
};
}

#endif
