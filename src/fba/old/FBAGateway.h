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
    // FBAQuorumSet/Envelope receival
    virtual void receiveQuorumSet(const FBAQuorumSet& qSet) = 0;
    virtual void receiveEnvelope(const FBAEnvelope& envelope) = 0;

    // Ballot preparation
    virtual bool prepareBallot(const FBASlotBallot& ballot) = 0;

    // Local QuorumSet interface
    virtual void setLocalQuorumSet(const QuorumSet& qset) = 0;
    virtual const QuorumSet& getLocalQuorumSet() = 0;
};
}

#endif
