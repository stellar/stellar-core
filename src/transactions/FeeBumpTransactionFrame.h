// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "transactions/TransactionFrame.h"

namespace stellar
{
class AbstractLedgerTxn;
class Application;
class SignatureChecker;

class FeeBumpTransactionFrame : public TransactionFrameBase
{
    TransactionEnvelope mEnvelope;
    TransactionResult mResult;

    TransactionFramePtr mInnerTx;

    Hash const& mNetworkID;
    mutable Hash mContentsHash;
    mutable Hash mFullHash;

    bool checkSignature(SignatureChecker& signatureChecker,
                        LedgerTxnEntry const& account, int32_t neededWeight);

    bool commonValidPreSeqNum(AbstractLedgerTxn& ltx, bool forApply);

    enum ValidationType
    {
        kInvalid,         // transaction is not valid at all
        kInvalidPostAuth, // transaction is invalid but one-time signers
                          // should be removed
        kFullyValid
    };

    ValidationType commonValid(SignatureChecker& signatureChecker,
                               AbstractLedgerTxn& ltxOuter,
                               SequenceNumber current, bool applying);

    bool isTooEarly(LedgerTxnHeader const& header) const;
    bool isTooLate(LedgerTxnHeader const& header) const;

    bool processSignatures(SignatureChecker& signatureChecker,
                           AbstractLedgerTxn& ltxOuter);

    bool removeAccountSigner(LedgerTxnHeader const& header,
                             LedgerTxnEntry& account,
                             SignerKey const& signerKey) const;

    void removeUsedOneTimeSignerKeys(SignatureChecker& signatureChecker,
                                     AbstractLedgerTxn& ltx) const;
    void removeUsedOneTimeSignerKeys(AbstractLedgerTxn& ltx,
                                     AccountID const& accountID,
                                     std::set<SignerKey> const& keys) const;

    void resetResults(LedgerHeader const& header, int64_t baseFee);

  public:
    FeeBumpTransactionFrame(Hash const& networkID,
                            TransactionEnvelope const& envelope);

    virtual ~FeeBumpTransactionFrame(){};

    bool apply(Application& app, AbstractLedgerTxn& ltx,
               TransactionMetaV1& meta) override;

    bool checkValid(AbstractLedgerTxn& ltxOuter,
                    SequenceNumber current) override;

    TransactionEnvelope const& getEnvelope() const override;

    int64_t getFeeBid() const override;
    int64_t getMinFee(LedgerHeader const& header) const override;
    int64_t getFee(LedgerHeader const& header, int64_t baseFee) const override;

    Hash const& getContentsHash() const override;
    Hash const& getFullHash() const override;
    Hash const& getInnerHash() const override;

    size_t getOperationCountForApply() const override;
    size_t getOperationCountForValidation() const override;

    TransactionResult& getResult() override;

    TransactionResultCode getResultCode() const override;

    TransactionResultPair getResultPair() const;

    SequenceNumber getSeqNum() const override;

    AccountID getFeeSourceID() const override;
    AccountID getSourceID() const override;

    void insertLedgerKeysToPrefetch(
        std::unordered_set<LedgerKey>& keys) const override;

    static TransactionEnvelope normalize(TransactionEnvelope const& envelope);

    void processFeeSeqNum(AbstractLedgerTxn& ltx, int64_t baseFee) override;

    StellarMessage toStellarMessage() const override;

    std::vector<TransactionFrameBasePtr> transactionsToApply() override;
};
}
