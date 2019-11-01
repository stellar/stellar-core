#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

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
class AbstractLedgerTxn;
class Application;
class Database;
class OperationFrame;
class LedgerManager;
class LedgerTxnEntry;
class LedgerTxnHeader;
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

    std::shared_ptr<LedgerEntry const> mCachedAccount;

    Hash const& mNetworkID;     // used to change the way we compute signatures
    mutable Hash mContentsHash; // the hash of the contents
    mutable Hash mFullHash;     // the hash of the contents and the sig.

    std::vector<std::shared_ptr<OperationFrame>> mOperations;

    LedgerTxnEntry loadSourceAccount(AbstractLedgerTxn& ltx,
                                     LedgerTxnHeader const& header);

    enum ValidationType
    {
        kInvalid,             // transaction is not valid at all
        kInvalidUpdateSeqNum, // transaction is invalid but its sequence number
                              // should be updated
        kInvalidPostAuth,     // transaction is invalid but its sequence number
                              // should be updated and one-time signers removed
        kFullyValid
    };

    virtual bool isTooEarly(LedgerTxnHeader const& header) const;
    virtual bool isTooLate(LedgerTxnHeader const& header) const;

    bool commonValidPreSeqNum(AbstractLedgerTxn& ltx, bool forApply);

    virtual bool isBadSeq(int64_t seqNum) const;

    ValidationType commonValid(SignatureChecker& signatureChecker,
                               AbstractLedgerTxn& ltxOuter,
                               SequenceNumber current, bool applying);

    virtual std::shared_ptr<OperationFrame>
    makeOperation(Operation const& op, OperationResult& res, size_t index);

    void resetResults(LedgerHeader const& header, int64_t baseFee);

    void removeUsedOneTimeSignerKeys(SignatureChecker& signatureChecker,
                                     AbstractLedgerTxn& ltx);

    void removeUsedOneTimeSignerKeys(AbstractLedgerTxn& ltx,
                                     AccountID const& accountID,
                                     std::set<SignerKey> const& keys) const;

    bool removeAccountSigner(LedgerTxnHeader const& header,
                             LedgerTxnEntry& account,
                             SignerKey const& signerKey) const;

    void markResultFailed();

    bool applyOperations(SignatureChecker& checker, Application& app,
                         AbstractLedgerTxn& ltx, TransactionMeta& meta);

    void processSeqNum(AbstractLedgerTxn& ltx);

    bool processSignatures(SignatureChecker& signatureChecker,
                           AbstractLedgerTxn& ltxOuter);

  public:
    TransactionFrame(Hash const& networkID,
                     TransactionEnvelope const& envelope);
    TransactionFrame(TransactionFrame const&) = delete;
    TransactionFrame() = delete;

    virtual ~TransactionFrame()
    {
    }

    static TransactionFramePtr
    makeTransactionFromWire(Hash const& networkID,
                            TransactionEnvelope const& msg);

    // clear pre-computed hashes
    void clearCached();

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

    AccountID const&
    getSourceID() const
    {
        return mEnvelope.tx.sourceAccount;
    }

    uint32_t getFeeBid() const;

    int64_t getMinFee(LedgerHeader const& header) const;

    virtual int64_t getFee(LedgerHeader const& header, int64_t baseFee) const;

    void addSignature(SecretKey const& secretKey);
    void addSignature(DecoratedSignature const& signature);

    bool checkSignature(SignatureChecker& signatureChecker,
                        LedgerTxnEntry const& account, int32_t neededWeight);

    bool checkSignatureNoAccount(SignatureChecker& signatureChecker,
                                 AccountID const& accountID);

    bool checkValid(AbstractLedgerTxn& ltxOuter, SequenceNumber current);

    // collect fee, consume sequence number
    virtual void processFeeSeqNum(AbstractLedgerTxn& ltx, int64_t baseFee);

    // apply this transaction to the current ledger
    // returns true if successfully applied
    bool apply(Application& app, AbstractLedgerTxn& ltx, TransactionMeta& meta);

    // version without meta
    bool apply(Application& app, AbstractLedgerTxn& ltx);

    StellarMessage toStellarMessage() const;

    LedgerTxnEntry loadAccount(AbstractLedgerTxn& ltx,
                               LedgerTxnHeader const& header,
                               AccountID const& accountID);

    // transaction history
    void storeTransaction(Database& db, uint32_t ledgerSeq, TransactionMeta& tm,
                          int txindex, TransactionResultSet& resultSet) const;

    // fee history
    void storeTransactionFee(Database& db, uint32_t ledgerSeq,
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

    static void deleteOldEntries(Database& db, uint32_t ledgerSeq,
                                 uint32_t count);
};
}
