#include "txherder/TxHerder.h"
#include "ledger/LedgerMaster.h"
#include "main/Application.h"
#include <time.h>
#include "lib/util/Logging.h"
#include "txherder/TransactionSet.h"
#include "lib/util/easylogging++.h"

#define MAX_TIME_IN_FUTURE 2

namespace stellar
{
	TxHerder::TxHerder()
	{
		mCloseCount = 0;
		mCollectingTransactionSet = TransactionSet::pointer(new TransactionSet());

		mReceivedTransactions.resize(4);
	}

	// make sure all the tx we have in the old set are included
	// make sure the timestamp isn't too far in the future
	TxHerderGateway::BallotValidType TxHerder::isValidBallotValue(Ballot::pointer ballot)
	{
		TransactionSetPtr txSet = fetchTxSet(ballot->mTxSetHash);
		if(!txSet)
		{
			LOG(ERROR) << "isValidBallotValue when we don't know the txSet";
			return INVALID_BALLOT;
		}

		for(auto tx : mReceivedTransactions[mReceivedTransactions.size() - 1])
		{
			if( find(txSet->mTransactions.begin(),txSet->mTransactions.end(),tx) == txSet->mTransactions.end())
				return INVALID_BALLOT;
		}
		// check timestamp
		if(ballot->mLedgerCloseTime <= mLastClosedLedger->mCloseTime) return INVALID_BALLOT;

		uint64_t timeNow = time(nullptr);
		if(ballot->mLedgerCloseTime > timeNow+MAX_TIME_IN_FUTURE) return FUTURE_BALLOT;

		return VALID_BALLOT; 
	}

	TxHerderGateway::SlotComparisonType TxHerder::compareSlot(Ballot::pointer ballot)
	{
		if(ballot->mLederIndex > mLastClosedLedger->mLedgerSeq) return(TxHerderGateway::FUTURE_SLOT);
		if(ballot->mLederIndex < mLastClosedLedger->mLedgerSeq) return(TxHerderGateway::PAST_SLOT);
		if(ballot->mPreviousLedgerHash == mLastClosedLedger->mHash) return(TxHerderGateway::SAME_SLOT);
		return TxHerderGateway::INCOMPATIBLIE_SLOT;
	}

	bool TxHerder::isTxKnown(stellarxdr::uint256& txHash)
	{
		for(auto list : mReceivedTransactions)
		{
			for(auto tx : list)
			{
				if(tx->getHash() == txHash) return true;
			}
		}

		return(false);
	}

	// a Tx set comes in from the wire
	void TxHerder::recvTransactionSet(TransactionSetPtr txSet)
	{
		if(mTxSetFetcher.recvItem(txSet))
		{
			for(auto tx : txSet->mTransactions)
			{
				recvTransaction(tx);
			}
			gApp.getFBAGateway().transactionSetAdded(txSet);
		}
	}

	// return true if we should flood
	bool TxHerder::recvTransaction(TransactionPtr tx)
	{
        stellarxdr::uint256 txHash = tx->getHash();
		if(!isTxKnown(txHash))
		{
			if(tx->isValid())
			{
				
				mReceivedTransactions[0].push_back(tx);

				return true;
			}
		}
		return false;
		
	}

	// will start fetching this TxSet from the network if we don't know about it
	TransactionSetPtr TxHerder::fetchTxSet(stellarxdr::uint256& setHash)
	{
		return mTxSetFetcher.fetchItem(setHash);
	}

	void TxHerder::removeReceivedTx(TransactionPtr dropTX)
	{
		for(auto list : mReceivedTransactions)
		{
			for(auto iter = list.begin(); iter != list.end();)
			{
				if((iter.operator->())->get()->getHash()==dropTX->getHash())
				{
					list.erase(iter);
					return;
				} else iter++;
			}
		}
	}

	// called by FBA
	void TxHerder::externalizeValue(Ballot::pointer ballot)
	{
		TransactionSet::pointer externalizedSet = fetchTxSet(ballot->mTxSetHash);
		if(externalizedSet)
		{
			gApp.getLedgerGateway().externalizeValue(externalizedSet, ballot->mLedgerCloseTime);

			// remove all these tx from mReceivedTransactions
			for(auto tx : externalizedSet->mTransactions)
			{
				removeReceivedTx(tx);
			}
			// move all the remaining to the next highest level
			// don't move the largest array
			for(int n = mReceivedTransactions.size() - 2; n >=0; n--)
			{
				for(auto tx : mReceivedTransactions[n])
				{
					mReceivedTransactions[n + 1].push_back(tx);
				}
				mReceivedTransactions[n].clear();
			}
		} else CLOG(ERROR, "TxHerder") << "externalizeValue txset not found: ";
	}

	// called by Ledger
	void TxHerder::ledgerClosed(LedgerPtr ledger)
	{
		// our first choice for this round's set is all the tx we have collected during last ledger close
		TransactionSet::pointer proposedSet(new TransactionSet());
		for( auto list : mReceivedTransactions )
		{
			for(auto tx : list)
			{
				proposedSet->add(tx);
			}
		}
		
		mLastClosedLedger = ledger;

		// we don't need to keep fetching any of the old TX sets
		mTxSetFetcher.clear();

		uint64_t firstBallotTime = time(nullptr)+1;
		if(firstBallotTime <= mLastClosedLedger->mCloseTime) firstBallotTime = mLastClosedLedger->mCloseTime + 1;

		recvTransactionSet(proposedSet);
		Ballot::pointer firstBallot(new Ballot(mLastClosedLedger, proposedSet->getContentsHash(), firstBallotTime));
		
		mCloseCount++;
		// don't participate in FBA for a few ledger closes so you make sure you don't send PREPAREs that don't include old tx
		if(mCloseCount > 2 && (!isZero(gApp.mConfig.VALIDATION_SEED))) gApp.getFBAGateway().setValidating(true);

		gApp.getFBAGateway().startNewRound(firstBallot);
	}

}
