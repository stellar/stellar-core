#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/InternalLedgerEntry.h"
#include "ledger/NetworkConfig.h"
#include "main/Config.h"
#include "overlay/StellarXDR.h"
#include "rust/RustBridge.h"
#include "transactions/TransactionFrameBase.h"
#include "transactions/TransactionMetaFrame.h"
#include "util/GlobalChecks.h"
#include "util/types.h"
#include "xdr/Stellar-ledger.h"

#include <memory>
#include <optional>
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

class TransactionResultPayload;
using TransactionResultPayloadPtr = std::shared_ptr<TransactionResultPayload>;

// TODO: Make private inner struct of TransactionResult
struct SorobanData
{
    xdr::xvector<ContractEvent> mEvents;
    xdr::xvector<DiagnosticEvent> mDiagnosticEvents;
    SCVal mReturnValue;
    // Size of the emitted Soroban events.
    uint32_t mConsumedContractEventsSizeBytes{};
    int64_t mFeeRefund{};
    int64_t mConsumedNonRefundableFee{};
    int64_t mConsumedRentFee{};
    int64_t mConsumedRefundableFee{};
    SorobanData()
    {
    }
};

// TODO: Make proper class
class TransactionResultPayload : NonMovableOrCopyable
{
  public:
    TransactionResultPayload(TransactionFrame& tx);

    TransactionResult txResult;
    std::vector<std::shared_ptr<OperationFrame>> opFrames;
    std::optional<TransactionResult> outerFeeBumpResult;
    std::optional<SorobanData> sorobanExtension;
};

class TransactionFrame : public TransactionFrameBase
{
  private:
    uint32_t getSize() const;

  protected:
    TransactionEnvelope mEnvelope;
    TransactionResult mResult;

    std::optional<SorobanData> mSorobanExtension;

    std::shared_ptr<InternalLedgerEntry const> mCachedAccount;

    Hash const& mNetworkID;     // used to change the way we compute signatures
    mutable Hash mContentsHash; // the hash of the contents
    mutable Hash mFullHash;     // the hash of the contents and the sig.

    // TODO: Remove, but for now have a redundant copy of the operations for
    // const state query (i.e. isSoroban)
    std::vector<std::shared_ptr<OperationFrame const>> mOperations;

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

    bool isTooEarlyForAccount(LedgerTxnHeader const& header,
                              LedgerTxnEntry const& sourceAccount,
                              uint64_t lowerBoundCloseTimeOffset) const;

    bool commonValidPreSeqNum(Application& app, AbstractLedgerTxn& ltx,
                              bool chargeFee,
                              uint64_t lowerBoundCloseTimeOffset,
                              uint64_t upperBoundCloseTimeOffset,
                              std::optional<FeePair> sorobanResourceFee,
                              TransactionResultPayload& resPayload);

    virtual bool isBadSeq(LedgerTxnHeader const& header, int64_t seqNum) const;

    ValidationType commonValid(Application& app,
                               SignatureChecker& signatureChecker,
                               AbstractLedgerTxn& ltxOuter,
                               SequenceNumber current, bool applying,
                               bool chargeFee,
                               uint64_t lowerBoundCloseTimeOffset,
                               uint64_t upperBoundCloseTimeOffset,
                               std::optional<FeePair> sorobanResourceFee,
                               TransactionResultPayload& resPayload);

    virtual std::shared_ptr<OperationFrame>
    makeOperation(Operation const& op, OperationResult& res, size_t index);

    void removeOneTimeSignerFromAllSourceAccounts(AbstractLedgerTxn& ltx) const;

    void removeAccountSigner(AbstractLedgerTxn& ltxOuter,
                             AccountID const& accountID,
                             SignerKey const& signerKey) const;

    void markResultFailed(TransactionResultPayload& resPayload);

    bool applyOperations(SignatureChecker& checker, Application& app,
                         AbstractLedgerTxn& ltx, TransactionMetaFrame& meta,
                         TransactionResultPayload& resPayload,
                         Hash const& sorobanBasePrngSeed);

    virtual void processSeqNum(AbstractLedgerTxn& ltx);

    bool processSignatures(ValidationType cv,
                           SignatureChecker& signatureChecker,
                           AbstractLedgerTxn& ltxOuter,
                           TransactionResultPayload& resPayload);

    std::optional<TimeBounds const> const getTimeBounds() const;
    std::optional<LedgerBounds const> const getLedgerBounds() const;
    bool extraSignersExist() const;

    bool validateSorobanOpsConsistency() const;
    bool validateSorobanResources(SorobanNetworkConfig const& config,
                                  Config const& appConfig,
                                  uint32_t protocolVersion,
                                  TransactionResultPayload& resPayload);
    int64_t refundSorobanFee(AbstractLedgerTxn& ltx,
                             AccountID const& feeSource);
    void updateSorobanMetrics(Application& app);
#ifdef BUILD_TESTS
  public:
#endif
    FeePair
    computePreApplySorobanResourceFee(uint32_t protocolVersion,
                                      SorobanNetworkConfig const& sorobanConfig,
                                      Config const& cfg);

    void pushSimpleDiagnosticError(Config const& cfg, SCErrorType ty,
                                   SCErrorCode code, std::string&& message,
                                   xdr::xvector<SCVal>&& args,
                                   TransactionResultPayload& resPayload);

  public:
    TransactionFrame(Hash const& networkID,
                     TransactionEnvelope const& envelope);
    TransactionFrame(TransactionFrame const&) = delete;
    TransactionFrame() = delete;

    virtual ~TransactionFrame()
    {
    }

    Hash const& getFullHash() const override;
    Hash const& getContentsHash() const override;

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

    void pushContractEvents(xdr::xvector<ContractEvent> const& evts,
                            TransactionResultPayload& resPayload);
    void pushDiagnosticEvents(xdr::xvector<DiagnosticEvent> const& evts,
                              TransactionResultPayload& resPayload);
    void setReturnValue(SCVal const& returnValue,
                        TransactionResultPayload& resPayload);
    void pushDiagnosticEvent(DiagnosticEvent const& evt,
                             TransactionResultPayload& resPayload);
    void pushApplyTimeDiagnosticError(Config const& cfg, SCErrorType ty,
                                      SCErrorCode code, std::string&& message,
                                      TransactionResultPayload& resPayload,
                                      xdr::xvector<SCVal>&& args = {});
    void pushValidationTimeDiagnosticError(Config const& cfg, SCErrorType ty,
                                           SCErrorCode code,
                                           std::string&& message,
                                           TransactionResultPayload& resPayload,
                                           xdr::xvector<SCVal>&& args = {});
    xdr::xvector<DiagnosticEvent> const& getDiagnosticEvents() const override;

    TransactionEnvelope const& getEnvelope() const override;

#ifdef BUILD_TESTS
    TransactionEnvelope& getEnvelope();
    void clearCached() override;
    TransactionFrame& toTransactionFrame() override;
    TransactionFrame const& toTransactionFrame() const override;

    bool
    isTestTx() const override
    {
        return false;
    }
#endif

    [[noreturn]] FeeBumpTransactionFrame const&
    toFeeBumpTransactionFrame() const override;

    SequenceNumber getSeqNum() const override;

    AccountID getFeeSourceID() const override;
    AccountID getSourceID() const override;

    uint32_t getNumOperations() const override;
    Resource getResources(bool useByteLimitInClassic) const override;

    std::vector<Operation> const& getRawOperations() const override;

    int64_t getFullFee() const override;
    int64_t getInclusionFee() const override;

    virtual int64_t getFee(LedgerHeader const& header,
                           std::optional<int64_t> baseFee,
                           bool applying) const override;

    bool checkSignature(SignatureChecker& signatureChecker,
                        LedgerTxnEntry const& account, int32_t neededWeight);

    bool checkSignatureNoAccount(SignatureChecker& signatureChecker,
                                 AccountID const& accountID);
    bool checkExtraSigners(SignatureChecker& signatureChecker);

    bool checkValidWithOptionallyChargedFee(
        Application& app, AbstractLedgerTxn& ltxOuter,
        TransactionResultPayload& resPayload, SequenceNumber current,
        bool chargeFee, uint64_t lowerBoundCloseTimeOffset,
        uint64_t upperBoundCloseTimeOffset);
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

    void
    insertKeysForFeeProcessing(UnorderedSet<LedgerKey>& keys) const override;
    void insertKeysForTxApply(UnorderedSet<LedgerKey>& keys,
                              LedgerKeyMeter* lkMeter) const override;

    // collect fee, consume sequence number
    void processFeeSeqNum(AbstractLedgerTxn& ltx,
                          std::optional<int64_t> baseFee,
                          TransactionResultPayload& resPayload) override;

    // apply this transaction to the current ledger
    // returns true if successfully applied
    bool apply(Application& app, AbstractLedgerTxn& ltx,
               TransactionMetaFrame& meta, TransactionResultPayload& resPayload,
               bool chargeFee, Hash const& sorobanBasePrngSeed);
    bool apply(Application& app, AbstractLedgerTxn& ltx,
               TransactionMetaFrame& meta, TransactionResultPayload& resPayload,
               Hash const& sorobanBasePrngSeed = Hash{}) override;

    // Performs the necessary post-apply transaction processing.
    // This has to be called after both `processFeeSeqNum` and
    // `apply` have been called.
    // Currently this only takes care of Soroban fee refunds.
    void processPostApply(Application& app, AbstractLedgerTxn& ltx,
                          TransactionMetaFrame& meta) override;

    // TransactionFrame specific function that allows fee bumps to forward a
    // different account for the refund. It also returns the refund so
    // FeeBumpTransactionFrame can adjust feeCharged.
    int64_t processRefund(Application& app, AbstractLedgerTxn& ltx,
                          TransactionMetaFrame& meta,
                          AccountID const& feeSource);

    // version without meta
    bool apply(Application& app, AbstractLedgerTxn& ltx,
               TransactionResultPayload& resPayload,
               Hash const& sorobanBasePrngSeed);

    std::shared_ptr<StellarMessage const> toStellarMessage() const override;

    LedgerTxnEntry loadAccount(AbstractLedgerTxn& ltx,
                               LedgerTxnHeader const& header,
                               AccountID const& accountID);

    std::optional<SequenceNumber const> const getMinSeqNum() const override;
    Duration getMinSeqAge() const override;
    uint32 getMinSeqLedgerGap() const override;

    bool hasDexOperations() const override;

    bool isSoroban() const override;
    SorobanResources const& sorobanResources() const override;

    bool consumeRefundableSorobanResources(
        uint32_t contractEventSizeBytes, int64_t rentFee,
        uint32_t protocolVersion, SorobanNetworkConfig const& sorobanConfig,
        Config const& cfg, TransactionResultPayload& resPayload);

    static FeePair computeSorobanResourceFee(
        uint32_t protocolVersion, SorobanResources const& txResources,
        uint32_t txSize, uint32_t eventsSize,
        SorobanNetworkConfig const& sorobanConfig, Config const& cfg);
    virtual int64 declaredSorobanResourceFee() const override;
    virtual bool XDRProvidesValidFee() const override;

    // TODO: Remove
    friend class TransactionResultPayload;

#ifdef BUILD_TESTS
    friend class TransactionTestFrame;
#endif
};

#ifdef BUILD_TESTS
class TransactionTestFrame;
using TransactionTestFramePtr = std::shared_ptr<TransactionTestFrame>;

class TransactionTestFrame : public TransactionFrameBase
{
  private:
    // TODO: Make const
    TransactionFrameBasePtr mTransactionFrame;
    TransactionResultPayload mTransactionResultPayload;

    TransactionTestFrame(TransactionFrameBasePtr tx);

  public:
    static TransactionTestFramePtr fromTxFrame(TransactionFrameBasePtr txFrame);

    // Test only functions
    bool apply(Application& app, AbstractLedgerTxn& ltx,
               TransactionMetaFrame& meta,
               Hash const& sorobanBasePrngSeed = Hash{});

    bool checkValid(Application& app, AbstractLedgerTxn& ltxOuter,
                    SequenceNumber current, uint64_t lowerBoundCloseTimeOffset,
                    uint64_t upperBoundCloseTimeOffset);

    void processFeeSeqNum(AbstractLedgerTxn& ltx,
                          std::optional<int64_t> baseFee);

    void addSignature(SecretKey const& secretKey);
    void addSignature(DecoratedSignature const& signature);

    std::vector<std::shared_ptr<OperationFrame const>> const&
    getOperations() const;

    // Redefinitions of TransactionFrameBase functions
    bool apply(Application& app, AbstractLedgerTxn& ltx,
               TransactionMetaFrame& meta, TransactionResultPayload& resPayload,
               Hash const& sorobanBasePrngSeed = Hash{}) override;

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
    TransactionEnvelope& getEnvelope() override;

    // clear pre-computed hashes
    void clearCached() override;

    TransactionFrame& toTransactionFrame() override;
    TransactionFrame const& toTransactionFrame() const override;
    FeeBumpTransactionFrame const& toFeeBumpTransactionFrame() const override;

    // Returns the total fee of this transaction, including the 'flat',
    // non-market part.
    int64_t getFullFee() const override;
    // Returns the part of the full fee used to make decisions as to
    // whether this transaction should be included into ledger.
    int64_t getInclusionFee() const override;
    int64_t getFee(LedgerHeader const& header, std::optional<int64_t> baseFee,
                   bool applying) const override;

    Hash const& getContentsHash() const override;
    Hash const& getFullHash() const override;

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
    void insertKeysForTxApply(UnorderedSet<LedgerKey>& keys) const override;

    void processFeeSeqNum(AbstractLedgerTxn& ltx,
                          std::optional<int64_t> baseFee,
                          TransactionResultPayload& resPayload) override;

    void processPostApply(Application& app, AbstractLedgerTxn& ltx,
                          TransactionMetaFrame& meta) override;

    StellarMessage toStellarMessage() const override;

    bool hasDexOperations() const override;

    bool isSoroban() const override;
    SorobanResources const& sorobanResources() const override;
    xdr::xvector<DiagnosticEvent> const& getDiagnosticEvents() const override;
    int64 declaredSorobanResourceFee() const override;
    bool XDRProvidesValidFee() const override;

    bool
    isTestTx() const override
    {
        return true;
    }
};
#endif
}
