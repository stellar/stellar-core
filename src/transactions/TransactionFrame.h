#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/AccountFrame.h"
#include "ledger/LedgerManager.h"
#include "overlay/StellarXDR.h"
#include "util/types.h"

#include <memory>
#include <set>

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
class SignatureChecker;
class XDROutputFileStream;
class SHA256;

class TransactionFrame;
using TransactionFramePtr = std::shared_ptr<TransactionFrame>;

class TransactionFrame
{
  protected:
    TransactionEnvelope mEnvelope;
    TransactionResult mResult;

    AccountFrame::pointer mSigningAccount;

    void clearCached();
    Hash const& mNetworkID;     // used to change the way we compute signatures
    mutable Hash mContentsHash; // the hash of the contents
    mutable Hash mFullHash;     // the hash of the contents and the sig.

    std::vector<std::shared_ptr<OperationFrame>> mOperations;

    bool loadAccount(int ledgerProtocolVersion, LedgerDelta* delta,
                     Database& app);
    bool commonValid(SignatureChecker& signatureChecker, Application& app,
                     LedgerDelta* delta, SequenceNumber current);

    void resetSigningAccount();
    void resetResults();
    void removeUsedOneTimeSignerKeys(SignatureChecker& signatureChecker,
                                     LedgerDelta& delta,
                                     LedgerManager& ledgerManager);
    void removeUsedOneTimeSignerKeys(const AccountID& accountId,
                                     const std::set<SignerKey>& keys,
                                     LedgerDelta& delta,
                                     LedgerManager& ledgerManager) const;
    bool removeAccountSigner(const AccountFrame::pointer& account,
                             const SignerKey& signerKey,
                             LedgerManager& ledgerManager) const;
    void markResultFailed();

  public:
    TransactionFrame(Hash const& networkID,
                     TransactionEnvelope const& envelope);
    TransactionFrame(TransactionFrame const&) = delete;
    TransactionFrame() = delete;

    static TransactionFramePtr
    makeTransactionFromWire(Hash const& networkID,
                            TransactionEnvelope const& msg);

    Hash const& getFullHash() const;
    Hash const& getContentsHash() const;

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

    uint32_t getFee() const;

    int64_t getMinFee(LedgerManager const& lm) const;

    double getFeeRatio(LedgerManager const& lm) const;

    void addSignature(SecretKey const& secretKey);
    void addSignature(DecoratedSignature const& signature);

    bool checkSignature(SignatureChecker& signatureChecker,
                        AccountFrame& account, int32_t neededWeight);

    bool checkValid(Application& app, SequenceNumber current);

    // collect fee, consume sequence number
    void processFeeSeqNum(LedgerDelta& delta, LedgerManager& ledgerManager);

    // apply this transaction to the current ledger
    // returns true if successfully applied
    bool apply(LedgerDelta& delta, TransactionMeta& meta, Application& app);

    // version without meta
    bool apply(LedgerDelta& delta, Application& app);

    StellarMessage toStellarMessage() const;

    AccountFrame::pointer loadAccount(int ledgerProtocolVersion,
                                      LedgerDelta* delta, Database& app,
                                      AccountID const& accountID);

    // transaction history
    void storeTransaction(LedgerManager& ledgerManager, TransactionMeta& tm,
                          int txindex, TransactionResultSet& resultSet) const;

    // fee history
    void storeTransactionFee(LedgerManager& ledgerManager,
                             LedgerEntryChanges const& changes,
                             int txindex) const;

    // access to history tables
    static TransactionResultSet getTransactionHistoryResults(Database& db,
                                                             uint32 ledgerSeq);
    static std::vector<LedgerEntryChanges>
    getTransactionFeeMeta(Database& db, uint32 ledgerSeq);

    /*
    txOut: stream of TransactionHistoryEntry
    txResultOut: stream of TransactionHistoryResultEntry
    */
    static size_t copyTransactionsToStream(Hash const& networkID, Database& db,
                                           soci::session& sess,
                                           uint32_t ledgerSeq,
                                           uint32_t ledgerCount,
                                           XDROutputFileStream& txOut,
                                           XDROutputFileStream& txResultOut);
    static void dropAll(Database& db);

    static void deleteOldEntries(Database& db, uint32_t ledgerSeq);
};
}
