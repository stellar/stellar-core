#ifndef __TRANSACTION__
#define __TRANSACTION__
#include <memory>
#include "ledger/AccountEntry.h"
#include "generated/stellar.hh"
#include "TransactionResultCodes.h"
#include "lib/util/types.h"

/*
A transaction in its exploded form.
We can get it in from the DB or from the wire
*/
namespace stellar
{
    

	class Transaction
	{
	protected:
		uint16_t mType;
		stellarxdr::uint160 mAccountID;
		uint32_t mSequence;
		uint32_t mFee;
		uint32_t mMinLedger;  // window where this tx is valid
		uint32_t mMaxLedger;
		// mSignature;

		AccountEntry mSigningAccount;	

        stellarxdr::uint256 mHash;

		virtual TxResultCode doApply() = 0;
	public:
		typedef std::shared_ptr<Transaction> pointer;

		static Transaction::pointer makeTransactionFromDB();
		static Transaction::pointer makeTransactionFromWire(stellarxdr::TransactionEnvelope& msg);
        stellarxdr::uint256 getHash();
		bool isValid();

		// apply this transaction to the current ledger
		// SANITY: how will applying historical txs work?
        TxResultCode apply();

		void serialize(stellarxdr::uint256& hash, Blob& blobRet);

        void toXDR(stellarxdr::TransactionEnvelope& envelope);
	};

}
#endif