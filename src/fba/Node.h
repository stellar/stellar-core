#ifndef __NODE__
#define __NODE__

#include <memory>
#include <vector>
#include "generated/stellar.hh"
#include "fba/Statement.h"

/*
This is one other Node out there in he stellar network

*/
namespace stellar
{
	class Ballot;
	typedef std::shared_ptr<Ballot> BallotPtr;
	class Statement;
	typedef std::shared_ptr<Statement> StatementPtr;

	class Node
	{
	
	public:
		typedef std::shared_ptr<Node> pointer;

		enum RatState {
			UNKNOWN_STATE,
            RECHECK_STATE,
			NOTPLEDGING_STATE,
            Q_NOT_PLEDGING_STATE,
            Q_UNKNOWN_STATE,
			PLEDGING_STATE,			// node has pledged 
			RATIFIED_STATE			// this node has ratified
		};
		
		Statement::StatementType mState;

		stellarxdr::uint256 mNodeID;

        

		// a vector of each message type holding a vector of the messages this node has sent for each message
		std::vector< std::vector<StatementPtr> > mStatements;
		// list of statements this node has ratified
		StatementPtr mRatified[Statement::NUM_TYPES];
		// this means you have a Q that has pledged this ballot
		// if you have a Q that is willRatify then you can ratify
		StatementPtr mWillRaitify[Statement::NUM_TYPES];


		Node(stellarxdr::uint256 nodeID);

		Statement::StatementType getNodeState(){ return(mState); }

		// get the highest Prepare message we have gotten for this Node
		StatementPtr getHighestStatement(Statement::StatementType type);

		bool hasStatement(StatementPtr msg);

		// returns false if we already had this msg
		bool addStatement(StatementPtr msg);

        Node::RatState checkRatState(Statement::StatementType type, BallotPtr statement, int operationToken, int recheckCounter);
		
    private:
        // for the ratification check
        int mRecheckCounter;
        int mOperationToken;
        Node::RatState mRatState;

	};
}

#endif