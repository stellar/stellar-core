#include "FBAMaster.h"
#include "main/Application.h"
#include "lib/util/Logging.h"

/*

Timestamp:
    set to be <min close time> seconds after your first proposal if you are making one fresh.
    set to be the same as the highest ranked ballot
    lower bound is last close time
    upper bound is 2 seconds into the future


Prepare means:	I will commit this ballot if a quorum does as well
				I won't commit a lower ranked ballot

Commit means:	I think this ballot is the correct one and I will externalize that fact
			


Process:
	
	We send a prepare
	We collect prepares
	With each inbound msg we see if:
		a) are we in a stuck state
		b) should we now commit

	We keep track of any of our quorum that is committed

	The ballot we initially prepare depends on the following:
		1) Highest from your Q set that includes all past tx and has the most votes
		2) Highest you have heard that includes all past tx
		3) Yours
		4) you are Vblocked
		highest we have heard proposed
		highest we have heard proposed from our Q
		the one proposed from our Q with the most prepares behind it
		the one with all the old tx in it
		the one we collected

	We don't want to switch to a higher ballot too quickly after we prepare
	We don't want to switch to a higher ballot if it is close to being ratified

	need to make sure we have the tx set of any 

	We want to prepare the highest ballot that we hear of when we start 


	There are 3 levels of ballot:
	A) will always prepare the highest ranked
	B) will only prepare if a vblocking set prepares
	C) will never prepare

*/

/*
When do we send PREPARE A ?
	- We send the highest ballot we know about
	- If we hear of a higher one on the same index we send it
	- If we are vblocked by a ballot we switch to that one
	- If we are vblocked switch to the highest option

	- If a vblocking set of ours ups the index 

When do we send ABORTED A?
	-sent after PREPARE A has been ratified or
	-(if you have already sent COMMIT B )

When do we send COMMIT A?
	-We have sent PREPARE A or ABORTED A  and
	-A has been "prepared" (all ballots< have been aborted by a quorum)


When do we send COMMITTED A?
	-COMMIT A has been ratified
	-

When do we externalize?
	-COMMITTED A has been ratified




*/

namespace stellar
{

	FBAMaster::FBAMaster()
	{
		mValidatingNode = false;   
	}

    void FBAMaster::setApplication(Application::pointer app) 
    { 
        mApp = app; 
        mOurNode = std::make_shared<OurNode>(mApp);
    }


	// start a new round of consensus. This is called after the last ledger closes
	void FBAMaster::startNewRound(Ballot::pointer firstBallot)
	{
		// start with all nodes having no FBA messages
		mKnownNodes.clear();
        mWaitFutureStatements.clear(); // SANITY: do these get cleaned up right away or do we need to go through and call cancel on all the timers?
		
		//apply any FBA messages we collected before close
		//clear collected messages
		vector<Statement::pointer> oldStatements = mCollectedStatements;
		mCollectedStatements.clear();
		mOurNode->startNewRound(firstBallot);

		if(mValidatingNode) processStatements(oldStatements);
		else 
		{  // not a validating node just look at the committed msgs
			mOurNode->mState = Statement::COMMITTED_TYPE;
			bool progress = false;
			for(auto statement : oldStatements)
			{
				if(statement->getType() == Statement::COMMITTED_TYPE)
				{
					processStatement(statement);
					progress = true;
				}

			}
			if(progress) mOurNode->progressFBA();
		}
	}

	// get a Statement msg from the wire
	void FBAMaster::recvStatement(Statement::pointer statement)
	{
		if(mValidatingNode || statement->getType()==Statement::COMMITTED_TYPE)
		{
			if(processStatement(statement) && mOurNode->getNodeState() == statement->getType())
			{
				mOurNode->progressFBA();
			}
		}	
	}


	/*
	When we get a FBA statement from the network we need to do the following:
	1) check the sig
	2) check if it is for this slot
	3) check if this msg is new
	4) check if we have this tx set       (LATER: We could switch the order of 4 and 5a. We will forward less messages and could inadvertently sybil people if we do though)
	4a) Fetch tx set from network
	5) check if we care about this msg
	5a) It is from someone in our sub-commit Qtree AND
	5b) We are below PLEDGING_COMMIT
	6) check if the tx set is valid
	6a) does it include any tx that we are demanding to be in there

	We should only forward the PREPARE on if:  1), 2), 3), 4) and 6)
	We should only fetch the tx set (3a) if 1), 2), 3)
	Add the prepare message to the node if 1) 2) 3) 4) 6)
	We only need to look at the effects if 1) 2) 3) 4) 5) 6)
	*/
	// returns true if this statement might progress FBA
	bool FBAMaster::processStatement(Statement::pointer statement)
	{
		if(!statement->isSigValid()) return false;  // 1) yes   LATER we should doc any peer that sends us one of these

		TxHerderGateway::SlotComparisonType slotCompare = mApp->getTxHerderGateway().compareSlot(statement->mBallot);
		if(slotCompare==TxHerderGateway::SAME_SLOT)
		{  // we are on the same slot 2) yes
			bool newStatement = true;
			Node::pointer node = mKnownNodes[statement->mNodeID];
			if(node)
			{	// we already knew about this node
				newStatement = !(mKnownNodes[statement->mNodeID]->hasStatement(statement));
			} else
			{	// new node
				node = std::make_shared<Node>(statement->mNodeID);
				mKnownNodes[statement->mNodeID] = node;
			}

			if(newStatement)
			{ // 3) yes

				TransactionSet::pointer txSet = statement->fetchTxSet(mApp);
				if(txSet)
				{ // set is local 4) yes
					TxHerderGateway::BallotValidType validity = statement->checkValidity(mApp);
					
					if(validity==TxHerderGateway::INVALID_BALLOT)
					{
						CLOG(WARNING, "FBA") << "Some old TX missing from PREPARE ballot ";
						
					} else if(validity == TxHerderGateway::FUTURE_BALLOT)
					{
						mWaitFutureStatements.push_back(
                            std::make_shared<FutureStatement>(statement,mApp));
						return false;
					}// 6) yes

					mKnownNodes[statement->mNodeID]->addStatement(statement);
					if(validity==TxHerderGateway::VALID_BALLOT) 
                        mApp->getOverlayGateway().broadcastMessage(statement->mSignature);

					///////  DO THE THING
					return(true);
				} else
				{ // need to fetch the txset from the network
					mWaitTxStatements.push_back(statement);
				}

			} else
			{ // we already knew about this statement
				// 3) no
			}
		} else
		{	// Not for the current slot. If it is for a slot in the future save it
			if(slotCompare==TxHerderGateway::FUTURE_SLOT)
			{
				mCollectedStatements.push_back(statement);
			} else if(slotCompare==TxHerderGateway::INCOMPATIBLIE_SLOT)
			{
                std::string str;
				CLOG(WARNING, "FBA") << "Node: " << toStr(statement->mNodeID,str) << " on a different ledger(" << statement->mBallot->mLederIndex << " : " << toStr(statement->mBallot->mPreviousLedgerHash,str);
			}
		}
		return false;
	}

    void FBAMaster::statementReady(FutureStatement::pointer fstate)
    {
       
        auto iter = find(mWaitFutureStatements.begin(), mWaitFutureStatements.end(), fstate);
        mWaitFutureStatements.erase(iter);
        recvStatement(fstate->mStatement);
    }
	

	// Who would be waiting for Tx sets?
	// just the ballots
	// once we get a set we can check its validity
	void FBAMaster::transactionSetAdded(TransactionSet::pointer txSet)
	{
		vector<Statement::pointer> fetched;

		for(auto iter = mWaitTxStatements.begin(); iter != mWaitTxStatements.end();)
		{
			Statement::pointer waiting = *iter;

			if(waiting->mBallot->mTxSetHash==txSet->getContentsHash())
			{
				mWaitTxStatements.erase(iter);
				fetched.push_back(waiting);
			} else iter++;
		}

		processStatements(fetched);
	}

	// OPTIMIZE: this will lead to checking sigs multiple times
	void FBAMaster::processStatements(vector<Statement::pointer>& statementList)
	{
		bool needProgress = false;

		// feed them back in to see if they move FBA along or get further flooded
		for(auto statement : statementList)
		{
			if(processStatement(statement) &&
				statement->getType() == mOurNode->getNodeState()) needProgress = true;
		}

		if(needProgress)
			mOurNode->progressFBA();
	}

	void FBAMaster::addQuorumSet(QuorumSet::pointer qset)
	{
		// LATER: we should make sure we asked for and need this Qset. For now just assume we asked for it if someone sent it to us
		mOurNode->progressFBA();
	}


}
