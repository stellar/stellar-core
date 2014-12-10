#ifndef __QUORUMSET__
#define __QUORUMSET__

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

	Need to ensure that threshold is > 50% of the nodes or the network won't be confluent

	if it a fixed % of the nodes then
*/

/*
we need some function that walks the quorum graph applying and keeping track of where we have been so we don't duplicate

maybe we don't need to walk since we can just keep track of which Qs reference which Nodes
	When a node is updated we update all the Qs that depend on it and see if any change state because of that

*/


using namespace std;
namespace stellar
{
	class BallotSet
	{
	public:
		stellarxdr::SlotBallot mBallot;
		int mCount;
		BallotSet(const stellarxdr::SlotBallot& b): mBallot(b), mCount(1) { }
	};

	class QuorumSet
	{
        stellarxdr::uint256 mHash;

		Node::RatState checkPrepareRatState(Statement::pointer statement, int visitIndex);
	public:
		typedef shared_ptr<QuorumSet> pointer;
		
		vector<Node::pointer> mNodes;
		int mThreshold;
		
		QuorumSet();
        QuorumSet(stellarxdr::QuorumSet& qset, Application &app);

        stellarxdr::uint256 getHash();
		int getBlockingSize();  // returns the # of nodes it takes to block a Quorum

		void sortBallots(stellarxdr::FBAStatementType type, vector< BallotSet >& retList);
		
        BallotPtr getMostPopularBallot(stellarxdr::FBAStatementType type, bool checkValid, Application &app);
		Statement::pointer getHighestStatement(stellarxdr::FBAStatementType type, bool checkValid, Application &app);
		
        Node::RatState checkRatState(stellarxdr::FBAStatementType statementType, BallotPtr ballot,
            int operationToken, int recheckCounter,Application &app);
		


        void toXDR(stellarxdr::QuorumSet& qset);

	};
}

#endif