
#include <time.h>
#include "fba/OurNode.h"
#include "main/Application.h"
#include "lib/util/Logging.h"
#include "fba/PreparedStatement.h"
#include "fba/PrepareStatement.h"
#include "fba/CommitStatement.h"
#include "fba/CommittedStatement.h"

#define PREPARE_TIMEOUT_MS 1000


namespace stellar
{
	int gOperationToken = 0;

	
	OurNode::OurNode(Application::pointer app) : Node()
	{
        mApp = app;
	}
	

	void OurNode::startNewRound(Ballot::pointer firstBallot)
	{
		mState = Statement::PREPARE_TYPE;
		mPreferredBallot = firstBallot;

	}

	void OurNode::progressFBA()
	{
		QuorumSet::pointer qset = mApp->getFBAGateway().getOurQuorumSet();
		if(qset)
		{
			switch(getNodeState())
			{
			case Statement::UNKNOWN_TYPE:
			case Statement::PREPARE_TYPE:
				progressPrepare(qset);
				break;
			case Statement::PREPARED_TYPE:
				progressPrepared(qset);
				break;
			case Statement::COMMIT_TYPE:
				progressCommit(qset);
				break;
			case Statement::COMMITTED_TYPE:
				progressCommitted(qset);
				break;
			}
		} else
		{
			CLOG(ERROR, "FBA") << "OurNode::progressFBA   We don't know our own Qset?";
		}
	}

	// we have either timed out or never sent a PREPARE
	void OurNode::sendNewPrepare(QuorumSet::pointer qset)
	{
		// switch to the highest valid ballot that has been sent by our Q
		Statement::pointer highestQPrepare = qset->getHighestStatement(Statement::PREPARE_TYPE, true,mApp);
		if(highestQPrepare)
		{
			if(highestQPrepare->mBallot->mIndex == 1 && mPreferredBallot->compare(highestQPrepare->mBallot))
			{  // our preferred is higher than the highest we have found
				sendStatement(Statement::PREPARE_TYPE, mPreferredBallot);
			} else sendStatement(Statement::PREPARE_TYPE, highestQPrepare->mBallot);
			return;
		}

		// you are first!
		sendStatement(Statement::PREPARE_TYPE,mPreferredBallot);
	}

	void OurNode::sendStatement(Statement::StatementType type, Ballot::pointer ballot)
	{
		QuorumSet::pointer qset = mApp->getFBAGateway().getOurQuorumSet();

		Statement::pointer statement;
		switch(type)
		{
		case Statement::PREPARE_TYPE:
        {
            PrepareStatement* prepStatement = new PrepareStatement(mNodeID, qset->getHash(), ballot);
            // LATER need to add all our exceptions to the statement
            statement = Statement::pointer(prepStatement);
        }break;
		case Statement::PREPARED_TYPE:
			statement = std::make_shared<PreparedStatement>(mNodeID, qset->getHash(), ballot);
			break;
		case Statement::COMMIT_TYPE:
			statement = std::make_shared<CommitStatement>(mNodeID, qset->getHash(), ballot);
			break;
		case Statement::COMMITTED_TYPE:
			statement = std::make_shared<CommittedStatement>(mNodeID, qset->getHash(), ballot);
			break;
		}
		
		
		statement->sign();
        StellarMessagePtr msg(new stellarxdr::StellarMessage());
        msg->type(stellarxdr::FBA_MESSAGE);
        statement->toXDR(msg->fbaMessage());

		mApp->getOverlayGateway().broadcastMessage(msg, Peer::pointer());
		mTimeSent[type] = std::chrono::system_clock::now();
		mState = type;
		// need to see how this effects everything
		progressFBA();

	}
	

	/*

	process once we send PREPARE A

	1) Is our ballot timed out?
		a) switch to the highest in our quorum set
	2) Are we vblocked?
		a) jump to the highest valid ballot in the lowest index that isn't vblocked
		b) if there isn't such a ballot, create a higher ballot with the most popular value.
	3) We have never sent a PREPARE statement
		a) <implies none are vblocked at this point>
		b) jump to highest valid ballot
	4) Do we have quorum?
		a) send PREPARED 
	5) Is there a higher ballot out there on the same index?
		a) switch to it
	*/
	void OurNode::progressPrepare(QuorumSet::pointer qset)
	{
		 
		Statement::pointer ourHighestPrepare = getHighestStatement(Statement::PREPARE_TYPE);

		if( (!ourHighestPrepare) ||
			(chrono::system_clock::now() > (mTimeSent[Statement::PREPARE_TYPE] + chrono::milliseconds(PREPARE_TIMEOUT_MS))))
		{   // we haven't prepared anything yet  3)
			// or our last msg has timedout  1)
			sendNewPrepare(qset);
			return;
		}

		vector< BallotSet > sortedBallots;
		qset->sortBallots(Statement::PREPARE_TYPE, sortedBallots);

		int aboveUsCount = 0;
		for(unsigned int n = 0; n < sortedBallots.size(); n++)
		{
			BallotSet& ballotSet = sortedBallots[n];

			if(ballotSet.mBallot->compare(ourHighestPrepare->mBallot))
			{ // this  ballot is higher than ours
				aboveUsCount += ballotSet.mCount;
				if(aboveUsCount >= qset->getBlockingSize())
				{ // our ballot is vblocked
					// walk back and see what ballot we should jump to
					int targetIndex = ballotSet.mBallot->mIndex;
					Ballot::pointer bestChoice;
					for(int i = n; i >= 0; i--)
					{
						if(bestChoice && sortedBallots[i].mBallot->mIndex > targetIndex)
						{
							// we have found the highest on the targetIndex
							break;
						}
						if(mApp->getTxHerderGateway().isValidBallotValue(sortedBallots[i].mBallot))
						{
							bestChoice = sortedBallots[i].mBallot;
						}
					}
					if(!bestChoice)
					{ // we are vblocked by invalid ballots
						// 
						Ballot::pointer popularBallot = qset->getMostPopularBallot(Statement::PREPARE_TYPE, true,mApp);
						if(popularBallot)
						{
							bestChoice = popularBallot;
								
						} else
						{   // all ballots above us are invalid
							// need to send our best guess with a higher index
							if(mPreferredBallot->compareValue(sortedBallots[0].mBallot)) mPreferredBallot->mIndex = sortedBallots[0].mBallot->mIndex;
							else mPreferredBallot->mIndex = sortedBallots[0].mBallot->mIndex+1;

							sendStatement(Statement::PREPARE_TYPE, mPreferredBallot);
						}
					}
						
					if(bestChoice)
					{
						bestChoice->mIndex = targetIndex;
						sendStatement(Statement::PREPARE_TYPE, bestChoice);
					}

				}
            } else
            { // we have already proposed something higher
                Ballot::pointer ratifiedBallot = whatRatified(Statement::PREPARE_TYPE);
                if(ratifiedBallot) sendStatement(Statement::PREPARED_TYPE, ratifiedBallot);  // SANITY: do we need to roll back some messages if we were going down a different path
            }
		}
	}

/*
	process once we send PREPARED A
	2) Are we vblocked?
		a) Jump back to prepare phase
	3) Do we have quorum?
		a) send COMMIT A

	LATER: we can factor out the commonalities between progressPrepared and progressCommit but let's wait till it is settled
*/

	void OurNode::progressPrepared(QuorumSet::pointer qset)
	{
		
		Statement::pointer ourHighestPrepared = getHighestStatement(Statement::PREPARED_TYPE);

		if(!ourHighestPrepared)
		{
			CLOG(ERROR, "FBA") << "OurNode::progressPrepared   ourHighestPrepared NULL?";
			return;
		}

		vector< BallotSet > sortedBallots;
		qset->sortBallots(Statement::PREPARED_TYPE, sortedBallots);

		int aboveUsCount = 0;
		for(unsigned int n = 0; n < sortedBallots.size(); n++)
		{
			BallotSet& ballotSet = sortedBallots[n];

			if(ballotSet.mBallot->compare(ourHighestPrepared->mBallot))
			{ // this  ballot is higher than ours
				aboveUsCount += ballotSet.mCount;
				if(aboveUsCount >= qset->getBlockingSize())
				{ // our ballot is vblocked
					// jump back to the prepare phase
					mState = Statement::PREPARE_TYPE;
					progressPrepare(qset);
				}
			} else
			{ // we have already proposed something higher
                Ballot::pointer ratifiedBallot = whatRatified(Statement::PREPARED_TYPE);
                if(ratifiedBallot) sendStatement(Statement::COMMIT_TYPE, ratifiedBallot);  // SANITY: do we need to roll back some messages if we were going down a different path
			}
		}
		
	}


	/*
	process once we send COMMIT A
	1) Are we vblocked?
		a) Jump back to prepare phase
	3) Do we have quorum?
		a) send COMMITED A
	*/
	
	void OurNode::progressCommit(QuorumSet::pointer qset)
	{
		
		Statement::pointer ourHighestCommit = getHighestStatement(Statement::COMMIT_TYPE);

		if(!ourHighestCommit)
		{
			CLOG(ERROR, "FBA") << "OurNode::progressCommit   ourHighestCommit NULL?";
			return;
		}

		vector< BallotSet > sortedBallots;
		qset->sortBallots(Statement::PREPARED_TYPE, sortedBallots);

		int aboveUsCount = 0;
		for(unsigned int n = 0; n < sortedBallots.size(); n++)
		{
			BallotSet& ballotSet = sortedBallots[n];

			if(ballotSet.mBallot->compare(ourHighestCommit->mBallot))
			{ // this  ballot is higher than ours
				aboveUsCount += ballotSet.mCount;
				if(aboveUsCount >= qset->getBlockingSize())
				{ // our ballot is vblocked
					// jump back to the prepare phase
					mState = Statement::PREPARE_TYPE;
					progressPrepare(qset);
				}
			} else
			{ // we have already proposed something higher
				// see if you have a quorum that has ratified anything
				Ballot::pointer ratifiedBallot = whatRatified(Statement::COMMIT_TYPE);
				if(ratifiedBallot) sendStatement(Statement::COMMITTED_TYPE, ratifiedBallot);  // SANITY: do we need to roll back some messages if we were going down a different path
			}
		}
		
	}

	/*
		process once we send COMMITTED A
		1) Do we have quorum?
		a) externalize A
	*/ 
	void OurNode::progressCommitted(QuorumSet::pointer qset)
	{	
		Ballot::pointer ratifiedBallot = whatRatified(Statement::COMMITTED_TYPE);
		if(ratifiedBallot)
		{
			Statement::pointer ourHighestCommitted = getHighestStatement(Statement::COMMITTED_TYPE);

			if(ourHighestCommitted)
			{
				if(ourHighestCommitted->mBallot->isCompatible(ratifiedBallot)) mApp->getTxHerderGateway().externalizeValue(ratifiedBallot);
			} else
			{ // this is a non-validating node. Just check if our qset has ratified anything
				
				mApp->getTxHerderGateway().externalizeValue(ratifiedBallot);
			}
		}
	}

   

    Ballot::pointer OurNode::whatRatified(Statement::StatementType type)
    {
        QuorumSet::pointer qset = mApp->getFBAGateway().getOurQuorumSet();

        Ballot::pointer popularBallot = qset->getMostPopularBallot(type,true,mApp);

        int operationToken = gOperationToken++;
        int recheckIndex = 0;

        int yesVotes = 0;
        for(unsigned int n = 0; n < qset->mNodes.size(); n++)
        {
            Node::RatState state = qset->mNodes[n]->checkRatState(type, popularBallot, 
                operationToken, recheckIndex, mApp);
            if(state == PLEDGING_STATE || state == RATIFIED_STATE)
            {
                yesVotes++;
                if(yesVotes > qset->mThreshold)
                {
                    return popularBallot;
                }
            } else if(state == RECHECK_STATE)
            {
                n = 0;
                recheckIndex++;
                yesVotes = 0;
            }
        }

        return Ballot::pointer();
    }
}

