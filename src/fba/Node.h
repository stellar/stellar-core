#ifndef __NODE__
#define __NODE__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include <memory>
#include <vector>
#include "generated/StellarXDR.h"
#include "fba/Statement.h"
#include "fba/FBA.h"

/*
This is one other Node out there in the stellar network

*/
namespace stellar
{
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
		
        stellarxdr::FBAStatementType mState;

		stellarxdr::uint256 mNodeID;

        

		// a vector of each message type holding a vector of the messages this node has sent for each message
		std::vector< std::vector<StatementPtr> > mStatements;
		// list of statements this node has ratified
		StatementPtr mRatified[stellarxdr::FBAStatementType::UNKNOWN];
		// this means you have a Q that has pledged this ballot
		// if you have a Q that is willRatify then you can ratify
		StatementPtr mWillRaitify[stellarxdr::FBAStatementType::UNKNOWN];

		Node(const stellarxdr::uint256& nodeID);

        stellarxdr::FBAStatementType getNodeState(){ return(mState); }

		// get the highest Prepare message we have gotten for this Node
		StatementPtr getHighestStatement(stellarxdr::FBAStatementType type);

		bool hasStatement(StatementPtr msg);

		// returns false if we already had this msg
		bool addStatement(StatementPtr msg);

        Node::RatState checkRatState(stellarxdr::FBAStatementType type, BallotPtr ballot,
            int operationToken, int recheckCounter, Application &app);
		
    private:
        // for the ratification check
        int mRecheckCounter;
        int mOperationToken;
        Node::RatState mRatState;

	};
}

#endif
