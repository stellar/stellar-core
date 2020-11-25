#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/InternalLedgerEntry.h"
#include "overlay/StellarXDR.h"
#include "transactions/TransactionFrameBase.h"
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

class TransactionFrame : public TransactionFrameBase
{
  protected:
    TransactionEnvelope mEnvelope;
    TransactionResult mResult;

    std::shared_ptr<InternalLedgerEntry const> mCachedAccount;

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
        kMaybeValid
    };

    virtual bool isTooEarly(LedgerTxnHeader const& header,
                            uint64_t lowerBoundCloseTimeOffset) const;
    virtual bool isTooLate(LedgerTxnHeader const& header,
                           uint64_t upperBoundCloseTimeOffset) const;

    bool commonValidPreSeqNum(AbstractLedgerTxn& ltx, bool chargeFee,
                              uint64_t lowerBoundCloseTimeOffset,
                              uint64_t upperBoundCloseTimeOffset);

    virtual bool isBadSeq(int64_t seqNum) const;

    ValidationType commonValid(SignatureChecker& signatureChecker,
                               AbstractLedgerTxn& ltxOuter,
                               SequenceNumber current, bool applying,
                               bool chargeFee,
                               uint64_t lowerBoundCloseTimeOffset,
                               uint64_t upperBoundCloseTimeOffset);

    virtual std::shared_ptr<OperationFrame>
    makeOperation(Operation const& op, OperationResult& res, size_t index);

    void removeOneTimeSignerFromAllSourceAccounts(AbstractLedgerTxn& ltx) const;

    void removeAccountSigner(AbstractLedgerTxn& ltxOuter,
                             AccountID const& accountID,
                             SignerKey const& signerKey) const;

    void markResultFailed();

    bool applyOperations(SignatureChecker& checker, Application& app,
                         AbstractLedgerTxn& ltx, TransactionMeta& meta);

    virtual void processSeqNum(AbstractLedgerTxn& ltx);

    bool processSignatures(ValidationType cv,
                           SignatureChecker& signatureChecker,
                           AbstractLedgerTxn& ltxOuter);

  public:
    TransactionFrame(Hash const& networkID,
                     TransactionEnvelope const& envelope);
    TransactionFrame(TransactionFrame const&) = delete;
    TransactionFrame() = delete;

    virtual ~TransactionFrame()
    {
    }

    // clear pre-computed hashes
    void clearCached();

    Hash const& getFullHash() const override;
    Hash const& getContentsHash() const override;

    std::vector<std::shared_ptr<OperationFrame>> const&
    getOperations() const
    {
        // this can only be used on an initialized TransactionFrame
        assert(!mOperations.empty());
        return mOperations;
    }

    TransactionResult const&
    getResult() const
    {
        return mResult;
    }

    TransactionResult&
    getResult() override
    {
        return mResult;
    }

    TransactionResultCode
    getResultCode() const override
    {
        return getResult().result.code();
    }

    void resetResults(LedgerHeader const& header, int64_t baseFee,
                      bool applying);

    TransactionEnvelope const& getEnvelope() const override;
    TransactionEnvelope& getEnvelope();

    SequenceNumber getSeqNum() const override;

    AccountID getFeeSourceID() const override;
    AccountID getSourceID() const override;

    uint32_t getNumOperations() const override;

    int64_t getFeeBid() const override;

    int64_t getMinFee(LedgerHeader const& header) const override;

    virtual int64_t getFee(LedgerHeader const& header, int64_t baseFee,
                           bool applying) const override;

    void addSignature(SecretKey const& secretKey);
    void addSignature(DecoratedSignature const& signature);

    bool checkSignature(SignatureChecker& signatureChecker,
                        LedgerTxnEntry const& account, int32_t neededWeight);

    bool checkSignatureNoAccount(SignatureChecker& signatureChecker,
                                 AccountID const& accountID);

    bool checkValid(AbstractLedgerTxn& ltxOuter, SequenceNumber current,
                    bool chargeFee, uint64_t lowerBoundCloseTimeOffset,
                    uint64_t upperBoundCloseTimeOffset);
    bool checkValid(AbstractLedgerTxn& ltxOuter, SequenceNumber current,
                    uint64_t lowerBoundCloseTimeOffset,
                    uint64_t upperBoundCloseTimeOffset) override;

    void
    insertKeysForFeeProcessing(UnorderedSet<LedgerKey>& keys) const override;
    void insertKeysForTxApply(UnorderedSet<LedgerKey>& keys) const override;

    // collect fee, consume sequence number
    void processFeeSeqNum(AbstractLedgerTxn& ltx, int64_t baseFee) override;

    // apply this transaction to the current ledger
    // returns true if successfully applied
    bool apply(Application& app, AbstractLedgerTxn& ltx, TransactionMeta& meta,
               bool chargeFee);
    bool apply(Application& app, AbstractLedgerTxn& ltx,
               TransactionMeta& meta) override;

    // version without meta
    bool apply(Application& app, AbstractLedgerTxn& ltx);

    StellarMessage toStellarMessage() const override;

    LedgerTxnEntry loadAccount(AbstractLedgerTxn& ltx,
                               LedgerTxnHeader const& header,
                               AccountID const& accountID);
};
}
