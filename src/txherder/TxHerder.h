#ifndef __TXHERDER__
#define __TXHERDER__

#include <vector>
#include "overlay/ItemFetcher.h"
#include "txherder/TxHerderGateway.h"


/*
receives tx from network
keeps track of all the Tx Sets we know about
FBA checks it to see if a give set is valid
It tells FBA to start the next round
*/

namespace stellar
{
    class Application;

	class TxHerder : public TxHerderGateway
	{
		// the transactions that we have collected during ledger close
		TransactionSet::pointer mCollectingTransactionSet;

		// keep track of txs that didn't make it into last ledger.
		// be less and less likely to commit a ballot that doesn't include the old ones
		map<stellarxdr::uint256, uint32_t> mTransactionAgeMap;

		
		// 0- tx we got during ledger close
		// 1- one ledger ago. Will only validate a vblocking set
		// 2- two ledgers ago. Will only validate a vblock set and will rebroadcast
		// 3- three or more ledgers ago. Any set we validate must have these tx
		vector< vector<Transaction::pointer> > mReceivedTransactions;

		TxSetFetcher mTxSetFetcher[2];
        int mCurrentTxSetFetcher;

		int mCloseCount;
        Application &mApp;

		LedgerPtr mLastClosedLedger;
		void removeReceivedTx(TransactionPtr tx);
	public:
		TxHerder(Application &app);

		///////// GATEWAY FUNCTIONS
		// make sure this set contains any super old TXs
		BallotValidType isValidBallotValue(BallotPtr ballot);
		TxHerderGateway::SlotComparisonType compareSlot(BallotPtr ballot);
		
		// will start fetching this TxSet from the network if we don't know about it
		TransactionSetPtr fetchTxSet(stellarxdr::uint256& setHash, bool askNetwork);

		void externalizeValue(BallotPtr ballot);

		// a Tx set comes in from the wire
		void recvTransactionSet(TransactionSetPtr txSet);
        void doesntHaveTxSet(stellarxdr::uint256& setHash, Peer::pointer peer) { mTxSetFetcher[mCurrentTxSetFetcher].doesntHave(setHash, peer);  }

		// we are learning about a new transaction
        // return true if we should flood
		bool recvTransaction(TransactionPtr tx);

		bool isTxKnown(stellarxdr::uint256& txHash);

		void ledgerClosed(LedgerPtr ledger);
		
		/////////////////

	};
}

#endif
