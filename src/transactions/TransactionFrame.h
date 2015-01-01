#ifndef __TRANSACTIONFRAME__
#define __TRANSACTIONFRAME__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC


#include <memory>
#include "ledger/AccountFrame.h"
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

	class TransactionFrame
	{
	protected:
        TransactionEnvelope mEnvelope;
		AccountFrame mSigningAccount;	
        uint256 mHash;
        TxResultCode mResultCode;


        bool preApply(TxDelta& delta, LedgerMaster& ledgerMaster);

        virtual bool doCheckValid(Application& app) = 0;
		virtual void doApply(TxDelta& delta, LedgerMaster& ledgerMaster) = 0;

        
        int64_t getTransferRate(Currency& currency, LedgerMaster& ledgerMaster);
        
       

        //bool isAuthorizedToHold(const AccountEntry& accountID, const CurrencyIssuer& currency,LedgerMaster& ledgerMaster);
	public:
		typedef std::shared_ptr<TransactionFrame> pointer;

        TransactionFrame(const TransactionEnvelope& envelope);

		//static TransactionFramePtr makeTransactionFromDB();
		static TransactionFrame::pointer makeTransactionFromWire(TransactionEnvelope const& msg);
        uint256& getHash();
        uint512& getSignature();

        TxResultCode getResultCode() { return mResultCode;  }

        bool checkValid(Application& app);

		// apply this transaction to the current ledger
		// LATER: how will applying historical txs work?
        void apply(TxDelta& delta, LedgerMaster& ledgerMaster);

        void toXDR(Transaction& body);
        void toXDR(TransactionEnvelope& envelope);
        StellarMessage&& toStellarMessage();

       
	};

}
#endif
