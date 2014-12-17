#ifndef __FBAGATEWAY__
#define __FBAGATEWAY__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "fba/FBA.h"

/*
The public interface to the FBA module
*/

namespace stellar
{
class FBAGateway
{
  public:
    // called by TxHerder
    virtual void startNewRound(const stellarxdr::SlotBallot& firstBallot) = 0;

    // a bit gross. ideally FBA wouldn't know what it is holding
    virtual void transactionSetAdded(TransactionSet::pointer txSet) = 0;

    virtual void setValidating(bool validating) = 0;

    // called by Overlay
    virtual void addQuorumSet(QuorumSet::pointer qset) = 0;
    virtual void recvStatement(Statement::pointer statement) = 0;

    // called internally
    virtual QuorumSet::pointer getOurQuorumSet() = 0;
    virtual Node::pointer getNode(stellarxdr::uint256& nodeID) = 0;
    virtual void statementReady(FutureStatementPtr statement) = 0;
};
}

#endif
