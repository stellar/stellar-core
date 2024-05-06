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
    TransactionEnvelope mEnvelope;
    TransactionResult mResult;

    TransactionFramePtr mInnerTx;

    Hash const& mNetworkID;
    mutable Hash mContentsHash;
    mutable Hash mFullHash;

    bool checkSignature(SignatureChecker& signatureChecker,
                        LedgerTxnEntry const& account, int32_t neededWeight);

    bool commonValidPreSeqNum(AbstractLedgerTxn& ltx,
                              TransactionResultPayload& resPayload);

    enum ValidationType
    {
        kInvalid,         // transaction is not valid at all
        kInvalidPostAuth, // transaction is invalid but one-time signers
                          // should be removed
        kFullyValid
    };

    ValidationType commonValid(SignatureChecker& signatureChecker,
                               AbstractLedgerTxn& ltxOuter, bool applying,
                               TransactionResultPayload& resPayload);

    void removeOneTimeSignerKeyFromFeeSource(AbstractLedgerTxn& ltx) const;

  public:
    FeeBumpTransactionFrame(Hash const& networkID,
                            TransactionEnvelope const& envelope);
#ifdef BUILD_TESTS
    FeeBumpTransactionFrame(Hash const& networkID,
                            TransactionEnvelope const& envelope,
                            TransactionFramePtr innerTx);

    TransactionEnvelope& getEnvelope() override;
    void clearCached() override;
    TransactionFrame& toTransactionFrame() override;
    TransactionFrame const& toTransactionFrame() const override;

    bool
    isTestTx() const override
    {
        return false;
    }
#endif

    virtual ~FeeBumpTransactionFrame(){};

    bool apply(Application& app, AbstractLedgerTxn& ltx,
               TransactionMetaFrame& meta, TransactionResultPayload& resPayload,
               Hash const& sorobanBasePrngSeed) override;

    void processPostApply(Application& app, AbstractLedgerTxn& ltx,
                          TransactionMetaFrame& meta,
                          TransactionResultPayload& resPayload) override;

    bool checkValid(Application& app, AbstractLedgerTxn& ltxOuter,
                    TransactionResultPayload& resPayload,
                    SequenceNumber current, uint64_t lowerBoundCloseTimeOffset,
                    uint64_t upperBoundCloseTimeOffset) override;
    bool checkSorobanResourceAndSetError(
        Application& app, uint32_t ledgerVersion,
        TransactionResultPayload& resPayload) override;

    void resetResults(LedgerHeader const& header,
                      std::optional<int64_t> baseFee, bool applying,
                      TransactionResultPayload& resPayload) override;

    TransactionEnvelope const& getEnvelope() const override;
    FeeBumpTransactionFrame const& toFeeBumpTransactionFrame() const override;

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

    TransactionResult& getResult() override;
    TransactionResultCode getResultCode() const override;

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

    void processFeeSeqNum(AbstractLedgerTxn& ltx,
                          std::optional<int64_t> baseFee,
                          TransactionResultPayload& resPayload) override;

    std::shared_ptr<StellarMessage const> toStellarMessage() const override;

    static TransactionEnvelope
    convertInnerTxToV1(TransactionEnvelope const& envelope);

    bool hasDexOperations() const override;

    bool isSoroban() const override;
    SorobanResources const& sorobanResources() const override;
    xdr::xvector<DiagnosticEvent> const& getDiagnosticEvents() const override;
    virtual int64 declaredSorobanResourceFee() const override;
    virtual bool XDRProvidesValidFee() const override;
};
}
