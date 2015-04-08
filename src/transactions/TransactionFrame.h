#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <memory>
#include "ledger/LedgerManager.h"
#include "ledger/AccountFrame.h"
#include "generated/StellarXDR.h"
#include "util/types.h"

namespace soci
{
class session;
}

/*
A transaction in its exploded form.
We can get it in from the DB or from the wire
*/
namespace stellar
{
class Application;
class OperationFrame;
class LedgerDelta;
class SecretKey;
class XDROutputFileStream;
class SHA256;

class TransactionFrame
{
  protected:
    TransactionEnvelope mEnvelope;
    TransactionResult mResult;

    AccountFrame::pointer mSigningAccount;
    std::vector<bool> mUsedSignatures;

    void clearCached();
    mutable Hash mContentsHash; // the hash of the contents
    mutable Hash mFullHash;     // the hash of the contents and the sig.

    std::vector<std::shared_ptr<OperationFrame>> mOperations;

    // collect fee, consume sequence number
    void prepareResult(LedgerDelta& delta, LedgerManager& ledgerManager);

    bool loadAccount(Application& app);
    bool checkValid(Application& app, bool applying, SequenceNumber current);

    void resetState();
    bool checkAllSignaturesUsed();
    void markResultFailed();

  public:
    typedef std::shared_ptr<TransactionFrame> pointer;

    TransactionFrame(TransactionEnvelope const& envelope);
    TransactionFrame(TransactionFrame const&) = delete;
    TransactionFrame() = delete;

    static TransactionFrame::pointer
    makeTransactionFromWire(TransactionEnvelope const& msg);

    Hash const& getFullHash() const;
    Hash const& getContentsHash() const;

    AccountFrame::pointer
    getSourceAccountPtr() const
    {
        return mSigningAccount;
    }

    void setSourceAccountPtr(AccountFrame::pointer signingAccount);

    std::vector<std::shared_ptr<OperationFrame>> const&
    getOperations() const
    {
        return mOperations;
    }

    TransactionResult const&
    getResult() const
    {
        return mResult;
    }

    TransactionResult&
    getResult()
    {
        return mResult;
    }

    TransactionResultCode
    getResultCode() const
    {
        return getResult().result.code();
    }

    TransactionResultPair getResultPair() const;
    TransactionEnvelope const& getEnvelope() const;
    TransactionEnvelope& getEnvelope();

    SequenceNumber
    getSeqNum() const
    {
        return mEnvelope.tx.seqNum;
    }

    AccountFrame const&
    getSourceAccount() const
    {
        assert(mSigningAccount);
        return *mSigningAccount;
    }

    AccountID const&
    getSourceID() const
    {
        return mEnvelope.tx.sourceAccount;
    }

    int64_t getFee(Application& app) const;

    void addSignature(SecretKey const& secretKey);

    bool checkSignature(AccountFrame& account, int32_t neededWeight);

    bool checkValid(Application& app, SequenceNumber current);

    // apply this transaction to the current ledger
    // returns true if successfully applied
    bool apply(LedgerDelta& delta, Application& app);

    StellarMessage toStellarMessage() const;

    AccountFrame::pointer loadAccount(Application& app,
                                      AccountID const& accountID);

    // transaction history
    void storeTransaction(LedgerManager& ledgerManager,
                          LedgerDelta const& delta, int txindex,
                          SHA256& resultHasher) const;

    /*
    txOut: stream of TransactionHistoryEntry
    txResultOut: stream of TransactionHistoryResultEntry
    */
    static size_t copyTransactionsToStream(Database& db, soci::session& sess,
                                           uint32_t ledgerSeq,
                                           uint32_t ledgerCount,
                                           XDROutputFileStream& txOut,
                                           XDROutputFileStream& txResultOut);
    static void dropAll(Database& db);
};
}
