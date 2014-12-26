#ifndef __TRANSACTION__
#define __TRANSACTION__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include <memory>
#include "ledger/AccountEntry.h"
#include "ledger/LedgerMaster.h"
#include "generated/StellarXDR.h"
#include "transactions/TxResultCode.h"
#include "util/types.h"
#include "lib/json/json-forwards.h"
#include "transactions/TxDelta.h"

/*
A transaction in its exploded form.
We can get it in from the DB or from the wire
*/
namespace stellar
{    
    class LedgerDelta;

	class Transaction
	{
	protected:
        stellarxdr::TransactionEnvelope mEnvelope;
		AccountEntry::pointer mSigningAccount;	
        stellarxdr::uint256 mHash;
        TxResultCode mResultCode;


        bool preApply(TxDelta& delta, LedgerMaster& ledgerMaster);

        virtual bool doCheckValid(LedgerMaster& ledgerMaster) = 0;
		virtual void doApply(TxDelta& delta, LedgerMaster& ledgerMaster) = 0;
	public:
		typedef std::shared_ptr<Transaction> pointer;

		static Transaction::pointer makeTransactionFromDB();
		static Transaction::pointer makeTransactionFromWire(stellarxdr::TransactionEnvelope const& msg);
        stellarxdr::uint256& getHash();
        stellarxdr::uint256& getSignature();
		bool isValid();

        TxResultCode getResultCode() { return mResultCode;  }

        bool checkValid(LedgerMaster& ledgerMaster);

		// apply this transaction to the current ledger
		// LATER: how will applying historical txs work?
        void apply(TxDelta& delta, LedgerMaster& ledgerMaster);

        void toXDR(stellarxdr::Transaction& body);
        void toXDR(stellarxdr::TransactionEnvelope& envelope);
        stellarxdr::StellarMessage&& toStellarMessage();

        static bool isNativeCurrency(stellarxdr::Currency currency);
	};

}
#endif
