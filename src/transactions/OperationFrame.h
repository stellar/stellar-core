// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "ledger/LedgerHashUtils.h"
#include "ledger/LedgerManager.h"
#include "ledger/NetworkConfig.h"
#include "main/AppConnector.h"
#include "overlay/StellarXDR.h"
#include "transactions/ParallelApplyUtils.h"
#include "util/types.h"
#include <memory>

namespace stellar
{
class AbstractLedgerTxn;
class LedgerManager;
class LedgerTxnEntry;
class LedgerTxnHeader;

class SignatureChecker;
class TransactionFrame;
class MutableTransactionResultBase;
class DiagnosticEventManager;
class RefundableFeeTracker;
class OperationMetaBuilder;
class ThreadParallelApplyLedgerState;

enum class ThresholdLevel
{
    LOW,
    MEDIUM,
    HIGH
};

class OperationFrame
{
  protected:
    Operation const& mOperation;
    TransactionFrame const& mParentTx;

    virtual bool
    doCheckValidForSoroban(SorobanNetworkConfig const& networkConfig,
                           Config const& appConfig, uint32_t ledgerVersion,
                           OperationResult& res,
                           DiagnosticEventManager& diagnosticEvents) const;
    virtual bool doCheckValid(uint32_t ledgerVersion,
                              OperationResult& res) const = 0;
    virtual bool
    doApplyForSoroban(AppConnector& app, AbstractLedgerTxn& ltx,
                      SorobanNetworkConfig const& sorobanConfig,
                      Hash const& sorobanBasePrngSeed, OperationResult& res,
                      std::optional<RefundableFeeTracker>& refundableFeeTracker,
                      OperationMetaBuilder& opMeta) const;
    virtual bool doApply(AppConnector& app, AbstractLedgerTxn& ltx,
                         OperationResult& res,
                         OperationMetaBuilder& opMeta) const = 0;

    virtual std::optional<ParallelTxSuccessVal>
    doParallelApply(AppConnector& app,
                    ThreadParallelApplyLedgerState const& threadState,
                    Config const& config, Hash const& txPrngSeed,
                    ParallelLedgerInfo const& ledgerInfo,
                    SorobanMetrics& sorobanMetrics, OperationResult& res,
                    std::optional<RefundableFeeTracker>& refundableFeeTracker,
                    OperationMetaBuilder& opMeta) const;

    // returns the threshold this operation requires
    virtual ThresholdLevel getThresholdLevel() const;

    // returns true if the operation is supported given a protocol version and
    // header flags
    virtual bool isOpSupported(LedgerHeader const& header) const;

    LedgerTxnEntry loadSourceAccount(AbstractLedgerTxn& ltx,
                                     LedgerTxnHeader const& header) const;

  public:
    static std::shared_ptr<OperationFrame>
    makeHelper(Operation const& op, TransactionFrame const& parentTx,
               uint32_t index);

    OperationFrame(Operation const& op, TransactionFrame const& parentTx);
    OperationFrame(OperationFrame const&) = delete;
    virtual ~OperationFrame() = default;

    // Verify signature requirements for this operation. Callers may set `res`
    // to `nullptr` if they do not directly need the result of signature
    // validation (such as in the case of background signature validation).
    bool checkSignature(SignatureChecker& signatureChecker,
                        LedgerSnapshot const& ls, OperationResult* res,
                        bool forApply) const;

    AccountID getSourceID() const;
    MuxedAccount getSourceAccount() const;

    bool checkValid(AppConnector& app, SignatureChecker& signatureChecker,
                    SorobanNetworkConfig const* cfg, LedgerSnapshot const& ls,
                    bool forApply, OperationResult& res,
                    DiagnosticEventManager& diagnosticEvents) const;

    bool apply(AppConnector& app, SignatureChecker& signatureChecker,
               AbstractLedgerTxn& ltx,
               std::optional<SorobanNetworkConfig const> const& sorobanConfig,
               Hash const& sorobanBasePrngSeed, OperationResult& res,
               std::optional<RefundableFeeTracker>& refundableFeeTracker,
               OperationMetaBuilder& opMeta) const;

    // Returns std::nullopt if operation fails.
    std::optional<ParallelTxSuccessVal> parallelApply(
        AppConnector& app, ThreadParallelApplyLedgerState const& threadState,
        Config const& config, ParallelLedgerInfo const& ledgerInfo,
        SorobanMetrics& sorobanMetrics, OperationResult& res,
        std::optional<RefundableFeeTracker>& refundableFeeTracker,
        OperationMetaBuilder& opMeta, Hash const& sorobanBasePrngSeed) const;

    Operation const&
    getOperation() const
    {
        return mOperation;
    }

    virtual void
    insertLedgerKeysToPrefetch(UnorderedSet<LedgerKey>& keys) const;

    virtual bool isDexOperation() const;

    virtual bool isSoroban() const;

    SorobanResources const& getSorobanResources() const;

    Memo const& getTxMemo() const;
    SorobanTransactionData::_ext_t const& getResourcesExt() const;
};
}
