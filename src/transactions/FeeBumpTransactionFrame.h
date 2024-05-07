// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "transactions/TransactionFrame.h"
#include "transactions/TransactionMetaFrame.h"

namespace stellar
{
class AbstractLedgerTxn;
class Application;
class SignatureChecker;

class FeeBumpTransactionFrame : public TransactionFrameBase
{
#ifdef BUILD_TESTS
    mutable
#else
    const
#endif
        TransactionEnvelope mEnvelope;
    TransactionFramePtr const mInnerTx;

    Hash const& mNetworkID;
    mutable Hash mContentsHash;
    mutable Hash mFullHash;

    bool checkSignature(SignatureChecker& signatureChecker,
                        LedgerTxnEntry const& account,
                        int32_t neededWeight) const;

    bool commonValidPreSeqNum(AbstractLedgerTxn& ltx,
                              MutableTransactionResultBase& txResult) const;

    enum ValidationType
    {
        kInvalid,         // transaction is not valid at all
        kInvalidPostAuth, // transaction is invalid but one-time signers
                          // should be removed
        kFullyValid
    };

    ValidationType commonValid(SignatureChecker& signatureChecker,
                               AbstractLedgerTxn& ltxOuter, bool applying,
                               MutableTransactionResultBase& txResult) const;

    void removeOneTimeSignerKeyFromFeeSource(AbstractLedgerTxn& ltx) const;

  public:
    FeeBumpTransactionFrame(Hash const& networkID,
                            TransactionEnvelope const& envelope);
#ifdef BUILD_TESTS
    FeeBumpTransactionFrame(Hash const& networkID,
                            TransactionEnvelope const& envelope,
                            TransactionFramePtr innerTx);

    TransactionEnvelope& getMutableEnvelope() const override;
    void clearCached() const override;

    bool
    isTestTx() const override
    {
        return false;
    }
#endif

    virtual ~FeeBumpTransactionFrame(){};

    bool apply(Application& app, AbstractLedgerTxn& ltx,
               TransactionMetaFrame& meta, TransactionResultPayloadPtr txResult,
               Hash const& sorobanBasePrngSeed) const override;

    void processPostApply(Application& app, AbstractLedgerTxn& ltx,
                          TransactionMetaFrame& meta,
                          TransactionResultPayloadPtr txResult) const override;

    std::pair<bool, TransactionResultPayloadPtr>
    checkValid(Application& app, AbstractLedgerTxn& ltxOuter,
               SequenceNumber current, uint64_t lowerBoundCloseTimeOffset,
               uint64_t upperBoundCloseTimeOffset) const override;
    bool checkSorobanResourceAndSetError(
        Application& app, uint32_t ledgerVersion,
        TransactionResultPayloadPtr txResult) const override;

    TransactionResultPayloadPtr createResultPayload() const override;

    TransactionResultPayloadPtr
    createResultPayloadWithFeeCharged(LedgerHeader const& header,
                                      std::optional<int64_t> baseFee,
                                      bool applying) const override;

    TransactionEnvelope const& getEnvelope() const override;

    int64_t getFullFee() const override;
    int64_t getInclusionFee() const override;
    int64_t getFee(LedgerHeader const& header, std::optional<int64_t> baseFee,
                   bool applying) const override;

    Hash const& getContentsHash() const override;
    Hash const& getFullHash() const override;
    Hash const& getInnerFullHash() const;

    uint32_t getNumOperations() const override;
    Resource getResources(bool useByteLimitInClassic) const override;

    std::vector<Operation> const& getRawOperations() const override;

    SequenceNumber getSeqNum() const override;
    AccountID getFeeSourceID() const override;
    AccountID getSourceID() const override;
    std::optional<SequenceNumber const> const getMinSeqNum() const override;
    Duration getMinSeqAge() const override;
    uint32 getMinSeqLedgerGap() const override;

    void
    insertKeysForFeeProcessing(UnorderedSet<LedgerKey>& keys) const override;
    void insertKeysForTxApply(UnorderedSet<LedgerKey>& keys,
                              LedgerKeyMeter* lkMeter) const override;

    TransactionResultPayloadPtr
    processFeeSeqNum(AbstractLedgerTxn& ltx,
                     std::optional<int64_t> baseFee) const override;

    std::shared_ptr<StellarMessage const> toStellarMessage() const override;

    static TransactionEnvelope
    convertInnerTxToV1(TransactionEnvelope const& envelope);

    bool hasDexOperations() const override;

    bool isSoroban() const override;
    SorobanResources const& sorobanResources() const override;
    virtual int64 declaredSorobanResourceFee() const override;
    virtual bool XDRProvidesValidFee() const override;
};
}
