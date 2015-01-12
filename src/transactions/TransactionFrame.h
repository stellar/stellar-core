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
    class Application;
    class LedgerDelta;
    class SecretKey;

	class TransactionFrame
	{
	protected:
        TransactionEnvelope mEnvelope;
		AccountFrame mSigningAccount;	
        Hash mContentsHash;  // the hash of the contents
        Hash mFullHash;    // the hash of the contents and the sig. 
        TxResultCode mResultCode;


        bool preApply(TxDelta& delta, LedgerMaster& ledgerMaster);
        bool checkSignature();

        virtual bool doCheckValid(Application& app) = 0;
		virtual bool doApply(TxDelta& delta, LedgerMaster& ledgerMaster) = 0;
        virtual int32_t getNeededThreshold();

        
        int64_t getTransferRate(Currency& currency, LedgerMaster& ledgerMaster);

	public:
		typedef std::shared_ptr<TransactionFrame> pointer;

        TransactionFrame(const TransactionEnvelope& envelope);

		static TransactionFrame::pointer makeTransactionFromWire(TransactionEnvelope const& msg);
        Hash& getFullHash();
        Hash& getContentsHash();
        uint32 getSeqNum() { return mEnvelope.tx.seqNum; }
        TransactionEnvelope& getEnvelope();
        AccountFrame& getSourceAccount() { return mSigningAccount;  }
        void addSignature(const SecretKey& secretKey);

        uint256& getSourceID() { return mEnvelope.tx.account; }

        bool loadAccount(Application& app);

        TxResultCode getResultCode() { return mResultCode;  }

        bool checkValid(Application& app);

		// apply this transaction to the current ledger
		// LATER: how will applying historical txs work?
        // returns true if successfully applied
        bool apply(TxDelta& delta, Application& app);

       
        StellarMessage&& toStellarMessage();

       
	};

}
#endif
