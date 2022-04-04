// Copyright 2020 Stellar Development Foundation and contributors. Licensed
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
  protected:
    TransactionEnvelope mEnvelope;
    TransactionFramePtr mInnerTx;

    Hash const& mNetworkID;
    mutable Hash mContentsHash;
    mutable Hash mFullHash;

    bool checkSignature(SignatureChecker& signatureChecker,
                        LedgerTxnEntry const& account, int32_t neededWeight);

    bool commonValidPreSeqNum(AbstractLedgerTxn& ltx,
                              TransactionResult& txResult);

    enum ValidationType
    {
        kInvalid,         // transaction is not valid at all
        kInvalidPostAuth, // transaction is invalid but one-time signers
                          // should be removed
        kFullyValid
    };

    ValidationType commonValid(SignatureChecker& signatureChecker,
                               AbstractLedgerTxn& ltxOuter, bool applying,
                               TransactionResult& txResult);

    void removeOneTimeSignerKeyFromFeeSource(AbstractLedgerTxn& ltx) const;

    void resetResults(LedgerHeader const& header, int64_t baseFee,
                      bool applying, TransactionResult& innerRes,
                      TransactionResult& outerRes);

  public:
    FeeBumpTransactionFrame(Hash const& networkID,
                            TransactionEnvelope const& envelope);

    virtual ~FeeBumpTransactionFrame(){};

    bool apply(Application& app, AbstractLedgerTxn& ltx, TransactionMeta& meta,
               TransactionResult& txResult) override;

    bool checkValid(AbstractLedgerTxn& ltxOuter, SequenceNumber current,
                    uint64_t lowerBoundCloseTimeOffset,
                    uint64_t upperBoundCloseTimeOffset,
                    TransactionResult& txResult) override;

    TransactionEnvelope const& getEnvelope() const override;

    int64_t getFeeBid() const override;
    int64_t getMinFee(LedgerHeader const& header) const override;
    int64_t getFee(LedgerHeader const& header, int64_t baseFee,
                   bool applying) const override;

    Hash const& getContentsHash() const override;
    Hash const& getFullHash() const override;
    Hash const& getInnerFullHash() const;

    uint32_t getNumOperations() const override;
    std::vector<Operation> const& getRawOperations() const override;
    SequenceNumber getSeqNum() const override;
    AccountID getFeeSourceID() const override;
    AccountID getSourceID() const override;
    std::optional<SequenceNumber const> const getMinSeqNum() const override;
    Duration getMinSeqAge() const override;
    uint32 getMinSeqLedgerGap() const override;

    void
    insertKeysForFeeProcessing(UnorderedSet<LedgerKey>& keys) const override;
    void insertKeysForTxApply(UnorderedSet<LedgerKey>& keys) const override;

    void processFeeSeqNum(AbstractLedgerTxn& ltx, int64_t baseFee,
                          TransactionResult& txResult) override;
    StellarMessage toStellarMessage() const override;

    static TransactionEnvelope
    convertInnerTxToV1(TransactionEnvelope const& envelope);

#ifdef BUILD_TESTS
    FeeBumpTransactionFrame(Hash const& networkID,
                            TransactionEnvelope const& envelope,
                            TransactionFramePtr innerTx);

    bool
    apply(Application& app, AbstractLedgerTxn& ltx,
          TransactionMeta& meta) override
    {
        throw std::runtime_error("Not implemented");
    }

    bool
    checkValid(AbstractLedgerTxn& ltxOuter, SequenceNumber current,
               uint64_t lowerBoundCloseTimeOffset,
               uint64_t upperBoundCloseTimeOffset) override
    {
        throw std::runtime_error("Not implemented");
    }

    TransactionResult&
    getResult() override
    {
        throw std::runtime_error("Not implemented");
    }

    TransactionResultCode
    getResultCode() const override
    {
        throw std::runtime_error("Not implemented");
    }

    void
    processFeeSeqNum(AbstractLedgerTxn& ltx, int64_t baseFee) override
    {
        throw std::runtime_error("Not implemented");
    }
#endif
};
}
