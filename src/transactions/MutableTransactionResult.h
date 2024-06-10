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

// This class holds all mutable state that is associated with a transaction.
class MutableTransactionResultBase : public NonMovableOrCopyable
{
  public:
    virtual TransactionResult& getResult() = 0;
    virtual TransactionResult const& getResult() const = 0;
    virtual TransactionResultCode getResultCode() const = 0;
    virtual void setResultCode(TransactionResultCode code) = 0;

    virtual OperationResult& getOpResultAt(size_t index) = 0;
    virtual xdr::xvector<DiagnosticEvent> const&
    getDiagnosticEvents() const = 0;

    virtual bool consumeRefundableSorobanResources(
        uint32_t contractEventSizeBytes, int64_t rentFee,
        uint32_t protocolVersion, SorobanNetworkConfig const& sorobanConfig,
        Config const& cfg, TransactionFrame const& tx) = 0;

    virtual void setSorobanConsumedNonRefundableFee(int64_t) = 0;
    virtual int64_t getSorobanFeeRefund() const = 0;
    virtual void setSorobanFeeRefund(int64_t fee) = 0;

    virtual void
    pushContractEvents(xdr::xvector<ContractEvent> const& evts) = 0;
    virtual void
    pushDiagnosticEvents(xdr::xvector<DiagnosticEvent> const& evts) = 0;
    virtual void setReturnValue(SCVal const& returnValue) = 0;
    virtual void pushDiagnosticEvent(DiagnosticEvent const& ecvt) = 0;
    virtual void pushSimpleDiagnosticError(Config const& cfg, SCErrorType ty,
                                           SCErrorCode code,
                                           std::string&& message,
                                           xdr::xvector<SCVal>&& args) = 0;
    virtual void
    pushApplyTimeDiagnosticError(Config const& cfg, SCErrorType ty,
                                 SCErrorCode code, std::string&& message,
                                 xdr::xvector<SCVal>&& args = {}) = 0;
    virtual void
    pushValidationTimeDiagnosticError(Config const& cfg, SCErrorType ty,
                                      SCErrorCode code, std::string&& message,
                                      xdr::xvector<SCVal>&& args = {}) = 0;
    virtual void publishSuccessDiagnosticsToMeta(TransactionMetaFrame& meta,
                                                 Config const& cfg) = 0;
    virtual void publishFailureDiagnosticsToMeta(TransactionMetaFrame& meta,
                                                 Config const& cfg) = 0;
};

class MutableTransactionResult : public MutableTransactionResultBase
{
  private:
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
    };

    TransactionResult mTxResult;
    std::optional<SorobanData> mSorobanExtension;

    MutableTransactionResult(TransactionFrame const& tx, int64_t feeCharged);

    friend TransactionResultPayloadPtr
    TransactionFrame::createResultPayload() const;

    friend TransactionResultPayloadPtr
    TransactionFrame::createResultPayloadWithFeeCharged(
        LedgerHeader const& header, std::optional<int64_t> baseFee,
        bool applying) const;

  public:
    virtual ~MutableTransactionResult() = default;

    TransactionResult& getResult() override;
    TransactionResult const& getResult() const override;
    TransactionResultCode getResultCode() const override;
    void setResultCode(TransactionResultCode code) override;

    OperationResult& getOpResultAt(size_t index) override;
    xdr::xvector<DiagnosticEvent> const& getDiagnosticEvents() const override;

    bool consumeRefundableSorobanResources(
        uint32_t contractEventSizeBytes, int64_t rentFee,
        uint32_t protocolVersion, SorobanNetworkConfig const& sorobanConfig,
        Config const& cfg, TransactionFrame const& tx) override;

    void setSorobanConsumedNonRefundableFee(int64_t fee) override;
    int64_t getSorobanFeeRefund() const override;
    void setSorobanFeeRefund(int64_t fee) override;

    void pushContractEvents(xdr::xvector<ContractEvent> const& evts) override;
    void
    pushDiagnosticEvents(xdr::xvector<DiagnosticEvent> const& evts) override;
    void setReturnValue(SCVal const& returnValue) override;
    void pushDiagnosticEvent(DiagnosticEvent const& ecvt) override;
    void pushSimpleDiagnosticError(Config const& cfg, SCErrorType ty,
                                   SCErrorCode code, std::string&& message,
                                   xdr::xvector<SCVal>&& args) override;
    void pushApplyTimeDiagnosticError(Config const& cfg, SCErrorType ty,
                                      SCErrorCode code, std::string&& message,
                                      xdr::xvector<SCVal>&& args = {}) override;
    void
    pushValidationTimeDiagnosticError(Config const& cfg, SCErrorType ty,
                                      SCErrorCode code, std::string&& message,
                                      xdr::xvector<SCVal>&& args = {}) override;
    void publishSuccessDiagnosticsToMeta(TransactionMetaFrame& meta,
                                         Config const& cfg) override;
    void publishFailureDiagnosticsToMeta(TransactionMetaFrame& meta,
                                         Config const& cfg) override;
};

class FeeBumpMutableTransactionResult : public MutableTransactionResultBase
{
    TransactionResult mTxResult;
    TransactionResultPayloadPtr mInnerResultPayload{};

    FeeBumpMutableTransactionResult(
        TransactionResultPayloadPtr innerResultPayload);

    friend TransactionResultPayloadPtr
    FeeBumpTransactionFrame::createResultPayload() const;

    friend TransactionResultPayloadPtr
    FeeBumpTransactionFrame::createResultPayloadWithFeeCharged(
        LedgerHeader const& header, std::optional<int64_t> baseFee,
        bool applying) const;

  public:
    virtual ~FeeBumpMutableTransactionResult() = default;

    // Updates outer fee bump result based on inner result.
    void updateResult(TransactionFrameBasePtr innerTx);

    void setInnerResultPayload(TransactionResultPayloadPtr innerResultPayload,
                               TransactionFrameBasePtr innerTx);

    TransactionResultPayloadPtr getInnerResultPayload();

    TransactionResult& getResult() override;
    TransactionResult const& getResult() const override;
    TransactionResultCode getResultCode() const override;
    void setResultCode(TransactionResultCode code) override;

    OperationResult& getOpResultAt(size_t index) override;
    xdr::xvector<DiagnosticEvent> const& getDiagnosticEvents() const override;

    bool consumeRefundableSorobanResources(
        uint32_t contractEventSizeBytes, int64_t rentFee,
        uint32_t protocolVersion, SorobanNetworkConfig const& sorobanConfig,
        Config const& cfg, TransactionFrame const& tx) override;

    void setSorobanConsumedNonRefundableFee(int64_t fee) override;
    int64_t getSorobanFeeRefund() const override;
    void setSorobanFeeRefund(int64_t fee) override;

    void pushContractEvents(xdr::xvector<ContractEvent> const& evts) override;
    void
    pushDiagnosticEvents(xdr::xvector<DiagnosticEvent> const& evts) override;
    void setReturnValue(SCVal const& returnValue) override;
    void pushDiagnosticEvent(DiagnosticEvent const& ecvt) override;
    void pushSimpleDiagnosticError(Config const& cfg, SCErrorType ty,
                                   SCErrorCode code, std::string&& message,
                                   xdr::xvector<SCVal>&& args) override;
    void pushApplyTimeDiagnosticError(Config const& cfg, SCErrorType ty,
                                      SCErrorCode code, std::string&& message,
                                      xdr::xvector<SCVal>&& args = {}) override;
    void
    pushValidationTimeDiagnosticError(Config const& cfg, SCErrorType ty,
                                      SCErrorCode code, std::string&& message,
                                      xdr::xvector<SCVal>&& args = {}) override;
    void publishSuccessDiagnosticsToMeta(TransactionMetaFrame& meta,
                                         Config const& cfg) override;
    void publishFailureDiagnosticsToMeta(TransactionMetaFrame& meta,
                                         Config const& cfg) override;
};
}