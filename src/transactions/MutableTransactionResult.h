#pragma once

// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/FeeBumpTransactionFrame.h"
#include "transactions/TransactionFrame.h"
#include "transactions/TransactionFrameBase.h"
#include "util/NonCopyable.h"
#include "util/types.h"

#include <memory>

namespace stellar
{

class Config;
class InternalLedgerEntry;
class SorobanNetworkConfig;

class SorobanTxData
{
  private:
    xdr::xvector<DiagnosticEvent> mDiagnosticEvents;
    xdr::xvector<ContractEvent> mEvents;
    SCVal mReturnValue;
    // Size of the emitted Soroban events.
    uint32_t mConsumedContractEventsSizeBytes{};
    int64_t mFeeRefund{};
    int64_t mConsumedNonRefundableFee{};
    int64_t mConsumedRentFee{};
    int64_t mConsumedRefundableFee{};

    void pushDiagnosticEvent(DiagnosticEvent const& ecvt);

  public:
    xdr::xvector<DiagnosticEvent> const& getDiagnosticEvents() const;

    bool consumeRefundableSorobanResources(
        uint32_t contractEventSizeBytes, int64_t rentFee,
        uint32_t protocolVersion, SorobanNetworkConfig const& sorobanConfig,
        Config const& cfg, TransactionFrame const& tx);

    void setSorobanConsumedNonRefundableFee(int64_t);
    int64_t getSorobanFeeRefund() const;
    void setSorobanFeeRefund(int64_t fee);

    void pushContractEvents(xdr::xvector<ContractEvent> const& evts);
    void setReturnValue(SCVal const& returnValue);

    void pushDiagnosticEvents(xdr::xvector<DiagnosticEvent> const& evts);
    void pushSimpleDiagnosticError(Config const& cfg, SCErrorType ty,
                                   SCErrorCode code, std::string&& message,
                                   xdr::xvector<SCVal>&& args);
    void pushApplyTimeDiagnosticError(Config const& cfg, SCErrorType ty,
                                      SCErrorCode code, std::string&& message,
                                      xdr::xvector<SCVal>&& args = {});
    void pushValidationTimeDiagnosticError(Config const& cfg, SCErrorType ty,
                                           SCErrorCode code,
                                           std::string&& message,
                                           xdr::xvector<SCVal>&& args = {});
    void publishSuccessDiagnosticsToMeta(TransactionMetaFrame& meta,
                                         Config const& cfg);
    void publishFailureDiagnosticsToMeta(TransactionMetaFrame& meta,
                                         Config const& cfg);
};

// This class holds all mutable state that is associated with a transaction.
class MutableTransactionResultBase : public NonMovableOrCopyable
{
  protected:
    std::unique_ptr<TransactionResult> mTxResult;
    std::optional<TransactionResult> mReplayTransactionResult{std::nullopt};

    MutableTransactionResultBase();
    MutableTransactionResultBase(MutableTransactionResultBase&& rhs);

  public:
    // For FeeBumpTxs, returns the inner TX result. For normal TXs, returns the
    // TX result.
    virtual TransactionResult& getInnermostResult() = 0;
    virtual void setInnermostResultCode(TransactionResultCode code) = 0;

    virtual TransactionResult& getResult() = 0;
    virtual TransactionResult const& getResult() const = 0;
    virtual TransactionResultCode getResultCode() const = 0;

    // Note: changing "code" normally causes the XDR structure to be destructed,
    // then a different XDR structure is constructed. However, txFAILED and
    // txSUCCESS have the same underlying field number so this does not
    // occur when changing between these two codes.
    virtual void setResultCode(TransactionResultCode code) = 0;
    virtual OperationResult& getOpResultAt(size_t index) = 0;
    virtual std::shared_ptr<SorobanTxData> getSorobanData() = 0;
    virtual xdr::xvector<DiagnosticEvent> const&
    getDiagnosticEvents() const = 0;

    virtual void refundSorobanFee(int64_t feeRefund,
                                  uint32_t ledgerVersion) = 0;
    virtual bool isSuccess() const = 0;
#ifdef BUILD_TESTS
    virtual std::optional<TransactionResult> const&
    getReplayTransactionResult() const = 0;
    virtual void
    setReplayTransactionResult(TransactionResult const& replayResult) = 0;
#endif
};

class MutableTransactionResult : public MutableTransactionResultBase
{
  private:
    std::shared_ptr<SorobanTxData> mSorobanExtension;

    MutableTransactionResult(TransactionFrame const& tx, int64_t feeCharged);

    friend MutableTxResultPtr TransactionFrame::createSuccessResult() const;

    friend MutableTxResultPtr
    TransactionFrame::createSuccessResultWithFeeCharged(
        LedgerHeader const& header, std::optional<int64_t> baseFee,
        bool applying) const;

  public:
    virtual ~MutableTransactionResult() = default;

    TransactionResult& getInnermostResult() override;
    void setInnermostResultCode(TransactionResultCode code) override;
    TransactionResult& getResult() override;
    TransactionResult const& getResult() const override;
    TransactionResultCode getResultCode() const override;
    void setResultCode(TransactionResultCode code) override;
#ifdef BUILD_TESTS
    void
    setReplayTransactionResult(TransactionResult const& replayResult) override;
    std::optional<TransactionResult> const&
    getReplayTransactionResult() const override;
#endif // BUILD_TESTS

    OperationResult& getOpResultAt(size_t index) override;
    std::shared_ptr<SorobanTxData> getSorobanData() override;
    xdr::xvector<DiagnosticEvent> const& getDiagnosticEvents() const override;

    void refundSorobanFee(int64_t feeRefund, uint32_t ledgerVersion) override;
    bool isSuccess() const override;
};

class FeeBumpMutableTransactionResult : public MutableTransactionResultBase
{
    // mTxResult is outer result
    MutableTxResultPtr const mInnerTxResult;

    FeeBumpMutableTransactionResult(MutableTxResultPtr innerTxResult);

    FeeBumpMutableTransactionResult(MutableTxResultPtr&& outerTxResult,
                                    MutableTxResultPtr&& innerTxResult,
                                    TransactionFrameBasePtr innerTx);

    friend MutableTxResultPtr
    FeeBumpTransactionFrame::createSuccessResult() const;

    friend MutableTxResultPtr
    FeeBumpTransactionFrame::createSuccessResultWithFeeCharged(
        LedgerHeader const& header, std::optional<int64_t> baseFee,
        bool applying) const;

    friend MutableTxResultPtr
    FeeBumpTransactionFrame::createSuccessResultWithNewInnerTx(
        MutableTxResultPtr&& outerResult, MutableTxResultPtr&& innerResult,
        TransactionFrameBasePtr innerTx) const;

  public:
    virtual ~FeeBumpMutableTransactionResult() = default;

    // Updates outer fee bump result based on inner result.
    static void updateResult(TransactionFrameBasePtr innerTx,
                             MutableTransactionResultBase& txResult);

    TransactionResult& getInnermostResult() override;
    void setInnermostResultCode(TransactionResultCode code) override;
    TransactionResult& getResult() override;
    TransactionResult const& getResult() const override;
    TransactionResultCode getResultCode() const override;
    void setResultCode(TransactionResultCode code) override;

    OperationResult& getOpResultAt(size_t index) override;
    std::shared_ptr<SorobanTxData> getSorobanData() override;
    xdr::xvector<DiagnosticEvent> const& getDiagnosticEvents() const override;

    void refundSorobanFee(int64_t feeRefund, uint32_t ledgerVersion) override;
    bool isSuccess() const override;

#ifdef BUILD_TESTS
    void
    setReplayTransactionResult(TransactionResult const& replayResult) override;
    std::optional<TransactionResult> const&
    getReplayTransactionResult() const override;
#endif // BUILD_TESTS
};
}