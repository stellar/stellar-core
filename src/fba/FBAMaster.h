#ifndef __FBAMASTER__
#define __FBAMASTER__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include <map>
#include "ledger/Ledger.h"
#include "txherder/TxSetFrame.h"
#include "fba/QuorumSet.h"
#include "fba/Statement.h"
#include "fba/OurNode.h"
#include "fba/FBAGateway.h"
#include "fba/FutureStatement.h"

/*
There is one FBAMaster that oversees the consensus process

As we learn about transactions we add them to the mCollectingTransactionSet
negotiate with peers till we decide on the current tx set
send this tx set to ledgermaster to be applied
start next ledger close


What triggers ledgerclose?
        people on your UNL start to close
        you have tx and enough time has passed



When a ledger closes

FBA:
        we see a prepare msg from someone
        we issue our own prepare msg


*/

namespace stellar
{
class Application;

class FBAMaster : public FBAGateway
{
    Application& mApp;
    bool mValidatingNode;
    OurNode::pointer mOurNode;
    QuorumSet::pointer mOurQuorumSet; // just store it as a ::pointer since the
                                      // rest of the app wants it this way

    // map of nodes we have gotten FBA messages from in this round
    // we save ones we don't care about in case they are on some yet unknown
    // Quorum Set
    map<uint256, Node::pointer> mKnownNodes;

    // Statements we have gotten from the network but are waiting to get the
    // txset of
    vector<Statement::pointer> mWaitTxStatements;

    // statements we have gotten with a ledger time too far in the future
    vector<FutureStatement::pointer> mWaitFutureStatements;

    // Collect any FBA messages we get for the next slot in case people are
    // closing before you are ready
    vector<Statement::pointer> mCollectedStatements;

    enum FBAState
    {
        WAITING, // we committed the last ledger so fast that we should wait a
                 // bit before closing the next one
        UNPREPARED,
        PREPARED,
        RATIFIED,
        COMMITED
    };

    // make sure we only send out our own FBA messages if we are a validator

    bool processStatement(Statement::pointer statement);
    void processStatements(vector<Statement::pointer>& statementList);

    void createOurQuroumSet();

  public:
    FBAMaster(Application& app);

    void
    setValidating(bool validating)
    {
        mValidatingNode = validating;
    }

    void startNewRound(const SlotBallot& firstBallot);

    void transactionSetAdded(TxSetFramePtr txSet);

    void addQuorumSet(QuorumSet::pointer qset);

    QuorumSet::pointer
    getOurQuorumSet()
    {
        return mOurQuorumSet;
    }
    Node::pointer getNode(uint256& nodeID);
    OurNode::pointer getOurNode();

    // get a new statement from the network
    void recvStatement(Statement::pointer statement);

    void statementReady(FutureStatement::pointer statement);
};
}

#endif
