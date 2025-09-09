#pragma once

// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/EventManager.h"
#include "transactions/TransactionFrame.h"

#include <memory>

namespace stellar
{

using TransactionResultConstPtr =
    std::shared_ptr<MutableTransactionResultBase const>;

// This class tracks the refundable resources and corresponding fees for a
// transaction.
// This can not be instantiated directly and is only accessible through
// MutableTransactionResult.
class RefundableFeeTracker
{
  public:
    // Consumes the refundable fees for the provided refundable resources, as
    // well as the rent fee.
    // Returns `false` when the consumed fees exceed the available refundable
    // fee for a transaction, which means that the transaction has to fail.
    // `diagnosticEvents` will contain a detailed error message in that case.
    bool consumeRefundableSorobanResources(
        uint32_t contractEventSizeBytes, int64_t rentFee,
        uint32_t protocolVersion, SorobanNetworkConfig const& sorobanConfig,
        Config const& cfg, TransactionFrame const& tx,
        DiagnosticEventManager& diagnosticEvents);
    // Returns the total fee refund to apply for transaction.
    int64_t getFeeRefund() const;
    // Returns the rent fee consumed so far.
    int64_t getConsumedRentFee() const;
    // Returns the total refundable fee consumed so far.
    int64_t getConsumedRefundableFee() const;

  private:
    friend class MutableTransactionResultBase;
    friend class FeeBumpMutableTransactionResult;

    explicit RefundableFeeTracker(int64_t maximumRefundableFee);

    // Resets all the consumed fees to 0, thus setting the refund to the
    // maximum possible value.
    // This should be used when the transaction fails.
    void resetConsumedFee();

    int64_t mMaximumRefundableFee{};

    uint32_t mConsumedContractEventsSizeBytes{};
    int64_t mConsumedRentFee{};
    int64_t mConsumedRefundableFee{};
};

// This class holds result of a transaction that can be modified during the
// apply/validation flows.
// This also manages the refundable fee tracker, as the refunds are tied to
// the execution result and charged fee tracking.
// Note, that this tries to limit the possible result mutations to the
// minimum necessary set of simple transitions. E.g. errors can only be set
// once and can't go to success, fee charged may only be modified by
// refunds etc.
class MutableTransactionResultBase
{
  protected:
    MutableTransactionResultBase(TransactionResultCode resultCode);

    TransactionResult mTxResult;
    std::optional<RefundableFeeTracker> mRefundableFeeTracker{};
#ifdef BUILD_TESTS
    virtual void copyReplayResultWithoutFeeCharged() = 0;
    std::optional<TransactionResult> mReplayTransactionResult{};
#endif

  public:
    virtual ~MutableTransactionResultBase() = default;

    // Initializes the refundable fee tracker. This is only necessary for the
    // transactions that support refunds, i.e. Soroban transactions.
    void initializeRefundableFeeTracker(int64_t totalRefundableFee);
    // Returns the refundable fee tracker, or `nullopt` for results that don't
    // support refunds.
    std::optional<RefundableFeeTracker>& getRefundableFeeTracker();

    // Returns the transaction result XDR. This should only be called once at
    // the very end of transaction processing flow.
    TransactionResult const& getXDR() const;

    // Returns the result code of the transaction.
    TransactionResultCode getResultCode() const;

    // Sets the error code for the transaction result.
    void setError(TransactionResultCode code);
    // Sets the error code for the transaction to txINSUFFICIENT_FEE and also
    // sets `feeCharged` to the provided value.
    // This is only used for the transaction acceptance flow to communicate
    // the fee necessary to get into tx queue.
    void setInsufficientFeeErrorWithFeeCharged(int64_t feeCharged);

    // Finalizes the fee charged for the transaction based on the refund
    // tracked by this result.
    virtual void finalizeFeeRefund(uint32_t ledgerVersion) = 0;

    // Gets the code of the innermost transaction result (that's always the
    // result of the regular transaction, not the fee bump).
    virtual TransactionResultCode getInnermostResultCode() const = 0;
    // Sets the error code for the innermost transaction result (that's always
    // the result of the regular transaction, not the fee bump).
    virtual void setInnermostError(TransactionResultCode code) = 0;

    // Returns the result of the operation at specified index.
    // This is only valid for the results that have operations and will trigger
    // an assertion for the results that don't have operations (typically
    // validation time errors).
    virtual OperationResult& getOpResultAt(size_t index) = 0;

    // Returns `true` if the result represents success.
    virtual bool isSuccess() const = 0;

    // Returns the fee charged for the transaction up to the point of this call.
    // Whether or not the refund is accounted for depends on if this is called
    // before or after the refund has been applied.
    int64_t getFeeCharged() const;

#ifdef BUILD_TESTS
    virtual std::unique_ptr<MutableTransactionResultBase> clone() const = 0;

    void overrideFeeCharged(int64_t feeCharged);
    void overrideXDR(TransactionResult const& resultXDR);

    // Stores the replayed result for this transaction, but doesn't mutate the
    // actual result.
    void setReplayTransactionResult(TransactionResult const& replayResult);
    // If there is a replayed transaction result, and it's a failure, then
    // modify the internal result to match the replayed result and return
    // `true`. Otherwise, do nothing and return `false`.
    bool adoptFailedReplayResult();
    // Returns `true` if there is a stored replayed transaction result.
    bool hasReplayTransactionResult() const;
#endif
};

// Result of a 'regular' (non-fee-bump) transaction.
class MutableTransactionResult : public MutableTransactionResultBase
{
  private:
    MutableTransactionResult(TransactionResultCode txErrorCode);
    MutableTransactionResult(TransactionFrame const& tx, int64_t feeCharged);

  protected:
#ifdef BUILD_TESTS
    void copyReplayResultWithoutFeeCharged() override;
#endif

  public:
    // Creates a transaction result with the provided error code.
    static std::unique_ptr<MutableTransactionResult>
    createTxError(TransactionResultCode txErrorCode);

    // Creates a successful transaction result with the provided fee charged.
    static std::unique_ptr<MutableTransactionResult>
    createSuccess(TransactionFrame const& tx, int64_t feeCharged);

    ~MutableTransactionResult() override = default;

    TransactionResultCode getInnermostResultCode() const override;
    void setInnermostError(TransactionResultCode code) override;

    void finalizeFeeRefund(uint32_t ledgerVersion) override;

    OperationResult& getOpResultAt(size_t index) override;

    bool isSuccess() const override;
#ifdef BUILD_TESTS
    std::unique_ptr<MutableTransactionResultBase> clone() const override;
#endif
};

// Result of a fee bump transaction.
class FeeBumpMutableTransactionResult : public MutableTransactionResultBase
{
  private:
    FeeBumpMutableTransactionResult(TransactionResultCode txErrorCode);
    FeeBumpMutableTransactionResult(TransactionFrame const& innerTx,
                                    int64_t feeCharged,
                                    int64_t innerFeeCharged);

    InnerTransactionResult& getInnerResult();
    InnerTransactionResult const& getInnerResult() const;

  protected:
#ifdef BUILD_TESTS
    void copyReplayResultWithoutFeeCharged() override;
#endif

  public:
    // Creates a fee bump transaction result with the provided error code.
    static std::unique_ptr<FeeBumpMutableTransactionResult>
    createTxError(TransactionResultCode txErrorCode);
    // Creates a successful fee bump transaction result with the provided fee
    // charged and fee charged for the inner transaction.
    static std::unique_ptr<FeeBumpMutableTransactionResult>
    createSuccess(TransactionFrame const& innerTx, int64_t feeCharged,
                  int64_t innerFeeCharged);

    ~FeeBumpMutableTransactionResult() override = default;

    TransactionResultCode getInnermostResultCode() const override;
    void setInnermostError(TransactionResultCode code) override;

    OperationResult& getOpResultAt(size_t index) override;

    bool isSuccess() const override;

    void finalizeFeeRefund(uint32_t protocolVersion) override;

#ifdef BUILD_TESTS
    std::unique_ptr<MutableTransactionResultBase> clone() const override;
#endif
};
}
