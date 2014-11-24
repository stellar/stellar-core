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
we need some function that walks the quroum graph applying and keeping track of where we have been so we don't duplicate

maybe we don't need to walk since we can just keep track of which Qs reference which Nodes
	When a node is updated we update all the Qs that depend on it and see if any change state because of that

*/


using namespace std;
namespace stellar
{
	class BallotSet
	{
	public:
		Ballot::pointer mBallot;
		int mCount;
		BallotSet(Ballot::pointer b){ mBallot = b; mCount = 1; }
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
        QuorumSet(stellarxdr::QuorumSet& qset);

        stellarxdr::uint256 getHash();
		int getBlockingSize();  // returns the # of nodes it takes to block a Quorum

		void sortBallots(Statement::StatementType type, vector< BallotSet >& retList);
		
		Ballot::pointer getMostPopularBallot(Statement::StatementType type, bool checkValid);
		Statement::pointer getHighestStatement(Statement::StatementType type, bool checkValid);
		bool checkQuorum(Statement::pointer statement);
        Node::RatState checkRatState(Statement::StatementType statementType, BallotPtr ballot, int operationToken, int recheckCounter);
		////


		// returns true if some Quorum of this Set has all pledged the given ballot 
		bool isAccepted(Ballot::pointer ballot);

        void toXDR(stellarxdr::QuorumSet& qset);
		

		bool isStuck();
		// returns true if this set cares and it changes what we already knew
		//bool prepare(Prepare::pointer prepare);
		//bool prepared(msg);
		//bool commit(msg);
		//bool commited(msg);
	};
}

#endif