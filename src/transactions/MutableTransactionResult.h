#pragma once

// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/EventManager.h"
#include "transactions/TransactionFrame.h"

#include <memory>

namespace stellar
{

class RefundableFeeTracker
{
  public:
    bool consumeRefundableSorobanResources(
        uint32_t contractEventSizeBytes, int64_t rentFee,
        uint32_t protocolVersion, SorobanNetworkConfig const& sorobanConfig,
        Config const& cfg, TransactionFrame const& tx,
        DiagnosticEventBuffer& diagnosticEvents);

    int64_t getFeeRefund() const;
    int64_t getConsumedRentFee() const;
    int64_t getConsumedRefundableFee() const;

  private:
    friend class MutableTransactionResultBase;
    friend class FeeBumpMutableTransactionResult;

    void reset();

    explicit RefundableFeeTracker(int64_t maximumRefundableFee);

    int64_t mMaximumRefundableFee{};

    uint32_t mConsumedContractEventsSizeBytes{};
    int64_t mConsumedRentFee{};
    int64_t mConsumedRefundableFee{};
};

// This class holds all mutable state that is associated with a transaction.
class MutableTransactionResultBase
{
  protected:
    MutableTransactionResultBase(TransactionResultCode resultCode);

    TransactionResult mTxResult;
    std::optional<TransactionResult> mReplayTransactionResult{};
    std::optional<RefundableFeeTracker> mRefundableFeeTracker{};

  public:
    void initializeRefundableFeeTracker(int64_t totalRefundableFee);
    std::optional<RefundableFeeTracker>& getRefundableFeeTracker();

    TransactionResult const& getXDR() const;

    TransactionResultCode getResultCode() const;
    // Note: changing "code" normally causes the XDR structure to be destructed,
    // then a different XDR structure is constructed. However, txFAILED and
    // txSUCCESS have the same underlying field number so this does not
    // occur when changing between these two codes.
    void setResultCode(TransactionResultCode code);

    int64_t getFeeCharged() const;

    void setInsufficientFeeErrorWithFeeCharged(int64_t feeCharged);

    virtual void finalizeFeeRefund(uint32_t ledgerVersion) = 0;

    // For FeeBumpTxs, gets/sets the result code for the inner TX result
    // For normal TXs, gets/sets the TX result code.
    virtual TransactionResultCode getInnermostResultCode() const = 0;
    virtual void setInnermostResultCode(TransactionResultCode code) = 0;

    // virtual TransactionResult const& getResult() const = 0;
    virtual OperationResult& getOpResultAt(size_t index) = 0;

    virtual bool isSuccess() const = 0;
    virtual ~MutableTransactionResultBase() = default;
#ifdef BUILD_TESTS
    virtual std::unique_ptr<MutableTransactionResultBase> clone() const = 0;

    void overrideFeeCharged(int64_t feeCharged);
    void overrideXDR(TransactionResult const& resultXDR);

    void setReplayTransactionResult(TransactionResult const& replayResult);
    bool adoptFailedReplayResult();
    bool hasReplayTransactionResult() const;
#endif
};

class MutableTransactionResult : public MutableTransactionResultBase
{
  private:
    MutableTransactionResult(TransactionResultCode txErrorCode);
    MutableTransactionResult(TransactionFrame const& tx, int64_t feeCharged);

  public:
    static std::unique_ptr<MutableTransactionResult>
    createTxError(TransactionResultCode txErrorCode);

    static std::unique_ptr<MutableTransactionResult>
    createSuccess(TransactionFrame const& tx, int64_t feeCharged);

    ~MutableTransactionResult() override = default;

    TransactionResultCode getInnermostResultCode() const override;
    void setInnermostResultCode(TransactionResultCode code) override;

    void finalizeFeeRefund(uint32_t ledgerVersion) override;

    OperationResult& getOpResultAt(size_t index) override;

    bool isSuccess() const override;

#ifdef BUILD_TESTS
    std::unique_ptr<MutableTransactionResultBase> clone() const override;
#endif
};

class FeeBumpMutableTransactionResult : public MutableTransactionResultBase
{
    int64_t mInnerFeeCharged = 0;

  private:
    FeeBumpMutableTransactionResult(TransactionResultCode txErrorCode);
    FeeBumpMutableTransactionResult(TransactionFrame const& innerTx,
                                    int64_t feeCharged,
                                    int64_t innerFeeCharged);

    InnerTransactionResult& getInnerResult();
    InnerTransactionResult const& getInnerResult() const;

  public:
    static std::unique_ptr<FeeBumpMutableTransactionResult>
    createTxError(TransactionResultCode txErrorCode);

    static std::unique_ptr<FeeBumpMutableTransactionResult>
    createSuccess(TransactionFrame const& innerTx, int64_t feeCharged,
                  int64_t innerFeeCharged);

    ~FeeBumpMutableTransactionResult() override = default;

    TransactionResultCode getInnermostResultCode() const override;
    void setInnermostResultCode(TransactionResultCode code) override;

    // TransactionResult const& getResult() const override;

    OperationResult& getOpResultAt(size_t index) override;

    bool isSuccess() const override;

    void finalizeFeeRefund(uint32_t protocolVersion) override;

#ifdef BUILD_TESTS
    std::unique_ptr<MutableTransactionResultBase> clone() const override;
#endif
};
}
