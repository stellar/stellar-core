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
        AccountFrame::pointer mSigningAccount;	
        Hash mContentsHash;  // the hash of the contents
        Hash mFullHash;    // the hash of the contents and the sig. 
        TxResultCode mResultCode;
        int64 mFee;


        // common side effects to all transactions (fees)
        // returns true if we should proceed with the transaction
        bool preApply(LedgerDelta& delta, LedgerMaster& ledgerMaster);

        bool checkSignature();

        virtual bool doCheckValid(Application& app) = 0;
        virtual bool doApply(LedgerDelta& delta, LedgerMaster& ledgerMaster) = 0;
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
        AccountFrame& getSourceAccount() { assert(mSigningAccount); return *mSigningAccount; }
        void addSignature(const SecretKey& secretKey);

        uint256& getSourceID() { return mEnvelope.tx.account; }

        // load account if needed
        // returns true on success
        bool loadAccount(Application& app);

        TxResultCode getResultCode() { return mResultCode;  }
        int64 getFee() { return mFee; }

        bool checkValid(Application& app);

        // apply this transaction to the current ledger
        // LATER: how will applying historical txs work?
        // returns true if successfully applied
        bool apply(LedgerDelta& delta, Application& app);

        StellarMessage&& toStellarMessage();

       
	};

}
#endif
