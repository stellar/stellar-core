#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include <memory>
#include "ledger/LedgerMaster.h"
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
class LedgerMaster;
class OperationFrame;
class LedgerDelta;
class SecretKey;
class XDROutputFileStream;
class SHA256;

class TransactionFrame
{
  protected:
    TransactionEnvelope mEnvelope;
    TransactionResultPair mResultPair;

    AccountFrame::pointer mSigningAccount;
    std::vector<bool> mUsedSignatures;

    Hash mContentsHash; // the hash of the contents
    Hash mFullHash;     // the hash of the contents and the sig.

    std::vector<std::shared_ptr<OperationFrame>> mOperations;

    // collect fee, consume sequence number
    void prepareResult(LedgerDelta& delta, LedgerMaster& ledgerMaster);

    bool loadAccount(Application& app);
    bool checkValid(Application& app, bool applying, SequenceNumber current);

    void resetState();
    bool checkAllSignaturesUsed();

  public:
    typedef std::shared_ptr<TransactionFrame> pointer;

    TransactionFrame(const TransactionEnvelope& envelope);
    TransactionFrame(TransactionFrame const&) = delete;
    TransactionFrame() = delete;

    static TransactionFrame::pointer
    makeTransactionFromWire(TransactionEnvelope const& msg);

    Hash& getFullHash();
    Hash& getContentsHash();

    AccountFrame::pointer
    getSourceAccountPtr()
    {
        return mSigningAccount;
    }
    void setSourceAccountPtr(AccountFrame::pointer signingAccount);
    std::vector<std::shared_ptr<OperationFrame>> const&
    getOperations()
    {
        return mOperations;
    }

    TransactionResult&
    getResult()
    {
        return mResultPair.result;
    }
    TransactionResultCode
    getResultCode()
    {
        return getResult().result.code();
    }

    TransactionResultPair& getResultPair();
    TransactionEnvelope& getEnvelope();

    SequenceNumber
    getSeqNum()
    {
        return mEnvelope.tx.seqNum;
    }
    AccountFrame&
    getSourceAccount()
    {
        assert(mSigningAccount);
        return *mSigningAccount;
    }
    uint256 const&
    getSourceID()
    {
        return mEnvelope.tx.account;
    }
    void addSignature(const SecretKey& secretKey);
    bool checkSignature(AccountFrame& account, int32_t neededWeight);

    bool checkValid(Application& app, SequenceNumber current);

    // apply this transaction to the current ledger
    // returns true if successfully applied
    bool apply(LedgerDelta& delta, Application& app);

    StellarMessage toStellarMessage();

    AccountFrame::pointer loadAccount(Application& app,
                                      uint256 const& accountID);

    // transaction history
    void storeTransaction(LedgerMaster& ledgerMaster, LedgerDelta const& delta,
                          int txindex, SHA256& resultHasher);

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
