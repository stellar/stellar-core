#include "QuorumSet.h"
#include "main/Application.h"

/*
Need to ensure that threshold is > 50% of the nodes or the network won't be confluent

*/

namespace stellar
{
	QuorumSet::QuorumSet()
	{
		mThreshold = 0;
	}

    // get qset from wire
    QuorumSet::QuorumSet(stellarxdr::QuorumSet& qset)
    {
        // SANITY
    }

    stellarxdr::uint256 QuorumSet::getHash()
    {
        // SANITY
        return mHash;
    }

	int QuorumSet::getBlockingSize()
	{
		return 1 + mNodes.size() - mThreshold;
	}

	//returns true if the first argument is ordered before the second. 
	bool ballotSorter(const BallotSet &a, const BallotSet &b)
	{
		return a.mBallot->compare(b.mBallot);
	}

	// returns all the ballots sorted by rank
	void QuorumSet::sortBallots(Statement::StatementType type, vector< BallotSet >& retList)
	{
		for(auto node : mNodes)
		{
			Statement::pointer statement = node->getHighestStatement(type);
			if(statement)
			{
				bool found = false;
				// check if this ballot is already on the list
				// check if any of the ballots are compatible with this one
				for(auto cballot : retList)
				{
					if(cballot.mBallot == statement->mBallot)
					{
						cballot.mCount++;
						found = true;
					}
				}
				if(!found) retList.push_back(BallotSet(statement->mBallot));
			}
		}
		// sort the list
		sort(retList.begin(), retList.end(), ballotSorter);
	}

	Ballot::pointer QuorumSet::getMostPopularBallot(Statement::StatementType type, bool checkValid)
	{
		map< pair<stellarxdr::uint256, uint64_t>, int> ballotCounts;
		Ballot::pointer ballot;

		for(auto node : mNodes)
		{
			Statement::pointer statement = node->getHighestStatement(type);
			if(statement)
			{
				if(!checkValid || gApp.getTxHerderGateway().isValidBallotValue(statement->mBallot))
				{
					ballot = statement->mBallot;
					ballotCounts[pair<stellarxdr::uint256, uint64_t>(ballot->mTxSetHash, ballot->mLedgerCloseTime)] += 1;
				}
			}
		}
		pair<stellarxdr::uint256, uint64_t> mostPopular;
		int mostPopularCount = 0;
		for(auto bcount : ballotCounts)
		{
			if(bcount.second > mostPopularCount)
			{
				mostPopular = bcount.first;
				mostPopularCount = bcount.second;
			}
		}

		if(ballot)
		{
			ballot=Ballot::pointer(new Ballot(ballot));
			ballot->mTxSetHash = mostPopular.first;
			ballot->mLedgerCloseTime = mostPopular.second;
			ballot->mIndex = 0;
			return ballot;
		}
		
		return Ballot::pointer();
	}

	// get the highest valid statement 
	Statement::pointer QuorumSet::getHighestStatement(Statement::StatementType type,bool checkValid)
	{
		Statement::pointer highStatement;
		for(auto node : mNodes)
		{
			Statement::pointer statement = node->getHighestStatement(type);
			if(!checkValid || gApp.getTxHerderGateway().isValidBallotValue(statement->mBallot))
			{
				if(!highStatement) highStatement = statement;
				else
				{
					if(statement && statement->compare(highStatement))
					{
						highStatement = statement;
					}
				}
			}
		}

		return highStatement;
	}

	// loop through the nodes and see if 
	// a) they are pledging a compatible ballot
	// b) they have ratified
	// for PREPARE we need to look at gaps
	//		for any gap see if other people can ratify the abort
    Node::RatState QuorumSet::checkRatState(Statement::StatementType statementType, BallotPtr ballot, int operationToken, int recheckCounter)
	{
		// LATER if(statementType == Statement::PREPARE_TYPE) return checkPrepareRatState(statement, visitIndex);
 
		int ratCount = 0;
		for(auto node : mNodes)
		{
			Node::RatState state = node->checkRatState(statementType, ballot, operationToken, recheckCounter);
			if(state == Node::PLEDGING_STATE || state == Node::RATIFIED_STATE)
			{
				ratCount++;  
                if(ratCount > mThreshold) return Node::PLEDGING_STATE;
			}
		}
		return Node::Q_NOT_PLEDGING_STATE;
	}

	// like checkRatState except we must take exceptions into account
	Node::RatState QuorumSet::checkPrepareRatState(Statement::pointer statement, int visitIndex)
	{
        // SANITY
        return Node::PLEDGING_STATE;
	}

    void QuorumSet::toXDR(stellarxdr::QuorumSet& qSet)
    {
        qSet.threshold = mThreshold;
        for(auto val : mNodes)
        {
            qSet.validators.push_back(val->mNodeID);
        }
    }
}