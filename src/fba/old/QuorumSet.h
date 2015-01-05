#ifndef __QUORUMSET__
#define __QUORUMSET__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "fba/Node.h"
#include "fba/Ballot.h"
//#include "fba/PrepareStatement.h"
#include "fba/Statement.h"

#include <vector>
/*
This is a set of nodes and a

How to represent a quorum set:
        - set of nodes and # that must vote for it
        - can the # be the same for the whole network?

        Need to ensure that threshold is > 50% of the nodes or the network won't
be confluent

        if it a fixed % of the nodes then
*/

/*
we need some function that walks the quorum graph applying and keeping track of
where we have been so we don't duplicate

maybe we don't need to walk since we can just keep track of which Qs reference
which Nodes
        When a node is updated we update all the Qs that depend on it and see if
any change state because of that

*/

using namespace std;
namespace stellar
{
class BallotSet
{
  public:
    SlotBallot mBallot;
    int mCount;
    BallotSet(const SlotBallot& b) : mBallot(b), mCount(1)
    {
    }
};

class QuorumSet
{
    uint256 mHash;

    Node::RatState checkPrepareRatState(Statement::pointer statement,
                                        int visitIndex);

  public:
    typedef shared_ptr<QuorumSet> pointer;

    vector<Node::pointer> mNodes;
    int mThreshold;

    QuorumSet();
    QuorumSet(QuorumSetDesc const& qset, Application& app);

    uint256 getHash();
    size_t getBlockingSize(); // returns the # of nodes it takes to block a Quorum

    void sortBallots(FBAStatementType type,
                     vector<BallotSet>& retList);

    BallotPtr getMostPopularBallot(FBAStatementType type,
                                   bool checkValid, Application& app);
    Statement::pointer getHighestStatement(FBAStatementType type,
                                           bool checkValid, Application& app);

    Node::RatState checkRatState(FBAStatementType statementType,
                                 BallotPtr ballot, int operationToken,
                                 int recheckCounter, Application& app);

    void toXDR(QuorumSetDesc& qset);
};
}

#endif
