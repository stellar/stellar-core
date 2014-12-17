#ifndef __TXHERDERGATEWAY__
#define __TXHERDERGATEWAY__

#include <memory>
#include "generated/StellarXDR.h"
#include "fba/FBA.h"
/*
Public Interface to the TXHerder module

Responsible for collecting Txs and TxSets from the network and making sure Txs aren't lost in ledger close


LATER: These gateway interface need cleaning up. We need to work out how to make the bidirectional interfaces

*/
namespace stellar
{
	class Ledger;
	typedef std::shared_ptr<Ledger> LedgerPtr;

	class TransactionSet;
	typedef std::shared_ptr<TransactionSet> TransactionSetPtr;

	class Transaction;
	typedef std::shared_ptr<Transaction> TransactionPtr;

    class Peer;
    typedef std::shared_ptr<Peer> PeerPtr;

	class TxHerderGateway
	{
	public:

		// called by FBA  LATER: shouldn't be here
		enum SlotComparisonType
		{
			SAME_SLOT,
			PAST_SLOT,
			FUTURE_SLOT,
			INCOMPATIBLIE_SLOT
		};
		enum BallotValidType
		{
			VALID_BALLOT,
			INVALID_BALLOT,
			FUTURE_BALLOT,
			UNKNOWN_VALIDITY
		};
		// make sure this set contains any super old TXs
		virtual BallotValidType isValidBallotValue(stellarxdr::Ballot const& ballot) = 0;
		virtual SlotComparisonType compareSlot(stellarxdr::SlotBallot const& ballot) = 0;

		// can start fetching this TxSet from the network if we don't know about it
		virtual TransactionSetPtr fetchTxSet(stellarxdr::uint256 const& setHash, bool askNetwork) = 0;

		virtual void externalizeValue(const stellarxdr::SlotBallot&) = 0;



		// called by Overlay
		// a Tx set comes in from the wire
		virtual void recvTransactionSet(TransactionSetPtr txSet) = 0;
        virtual void doesntHaveTxSet(stellarxdr::uint256 const& setHash, PeerPtr peer) = 0;

		// we are learning about a new transaction
		// returns true if we should flood this tx
		virtual bool recvTransaction(TransactionPtr tx) = 0;
		virtual bool isTxKnown(stellarxdr::uint256 const& txHash) = 0;


		//called by Ledger
		virtual void ledgerClosed(LedgerPtr ledger) = 0;

	};
}

#endif
