#include "Node.h"
#include "fba/Ballot.h"
#include "fba/QuorumSet.h"
#include "main/Application.h"

/*
A is ratified if:
    Va= some set of Nodes that have pledged A
    Every node in Va has a Quorum that is in A

    Should a node keep track of who depends on it so when its vote changes it can tell them?

    We trigger a recheck when some node flips from PLEDGING to Q_NOT_PLEDGING_STATE and that node has already been visited

How we check for ratification:
    we want to see if it is ratified from one node's point of view
    We start at the node we are interested in
    It asks all of its Qset what their state is
        Each node recursively checks. 
            If a node is visited a second time it will return its Pledge state
            If a node doesn't have a Q then we increment the recheckCounter and start the recursive search over


*/

namespace stellar
{
	Node::Node(const stellarxdr::uint256& nodeID) : mNodeID(nodeID)
	{
		mState = stellarxdr::FBAStatementType::UNKNOWN;
	}

	// called recursively
    // operationToken is unique for each checking of the ballot
    // recheckCounter is incremented during the course of one operation
    //  it will increment anytime a node is removed from Va thus triggering a recheck of all nodes in the "pledging" state
	
	// this returns what RatState it thinks the particular statement is in.
    Node::RatState Node::checkRatState(stellarxdr::FBAStatementType statementType, BallotPtr ballot,
                                       int operationToken, int recheckCounter, Application &app)
	{
        if(operationToken == mOperationToken)
        {   // this node was already visited during this check
            if( recheckCounter == mRecheckCounter ||
                mRatState != PLEDGING_STATE) return mRatState;
        } else
        {
            mRatState = UNKNOWN_STATE;  // first time through
        }
        mOperationToken = operationToken;
        mRecheckCounter = recheckCounter;


		for(unsigned n = statementType; n < stellarxdr::FBAStatementType::UNKNOWN; n++)
		{  // if this node has ratified this statement or a stronger version
            if(mRatified[n] && mRatified[n]->isCompatible(ballot))
            {
                mRatState = RATIFIED_STATE;
                return mRatState;
            }

			if(n>statementType)
			{ // check if this guy has already pledged a stronger statement
				Statement::pointer ourStatement = getHighestStatement((stellarxdr::FBAStatementType)n);
                if(ourStatement)
                {
                    if(ourStatement->isCompatible(ballot))
                    {// if we are pledging a stronger version that means we have ratified this lower version
                        mRatState = RATIFIED_STATE;
                    } else mRatState = NOTPLEDGING_STATE;   // SANITY is this correct to do this? We have pledged a different ballot at a higher level. 
                                                            // is it safe to say we are no longer pledging this lower level ballot?
                    return mRatState;
                }
			}	
		}

		Statement::pointer ourStatement = getHighestStatement(statementType);
        if(!ourStatement || !ourStatement->isCompatible(ballot))
        {
            mRatState = NOTPLEDGING_STATE;
            return mRatState;
        }
		
		// ok so this node is pledging the statement
        mRatState = PLEDGING_STATE;
		QuorumSet::pointer qset = app.getOverlayGateway().fetchQuorumSet(ourStatement->mEnvelope.contents.quorumSetHash,true);
		if(qset)
		{
            RatState state = qset->checkRatState(statementType, ballot, 
                operationToken, recheckCounter,app);
            if(state == RECHECK_STATE) return(RECHECK_STATE); // just bounce back to the top

			if(state == RATIFIED_STATE)
			{
				mRatified[statementType] = ourStatement;
			} 
            if(mRatState == PLEDGING_STATE && state==Q_NOT_PLEDGING_STATE)
            { // in the past we returned to someone that we are pledging so we need to recheck everything
                mRatState = state;
                return(RECHECK_STATE);
            }
            mRatState = state;
            
		} else
		{  // we haven't fetched this Qset but we know this guy at least is pledging
            mRatState = Q_UNKNOWN_STATE;
		}

        return mRatState;
	}


	bool Node::hasStatement(StatementPtr statement)
	{
		vector<StatementPtr>& v = mStatements[statement->getType()];
		if(find(v.begin(), v.end(), statement) == v.end())
		{ // not found	
			return(false);
		}
		return(true);
	}

	// returns false if we already had this statement
	bool Node::addStatement(StatementPtr statement)
	{
		vector<StatementPtr>& v = mStatements[statement->getType()];
		if(find(v.begin(), v.end(), statement) == v.end())
		{ // this is a new prepare msg
			v.push_back(statement);
			return(true);
		}
		return(false);
	}

	StatementPtr Node::getHighestStatement(stellarxdr::FBAStatementType type)
	{
		StatementPtr max;
		for(auto statement : mStatements[type])
		{
			if(max)
			{
				if(ballot::compare(statement->getBallot(),max->getBallot())) max = statement;
			} else max = statement;
		}
		return max;
	}
	
}
