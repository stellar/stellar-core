#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerHashUtils.h"
#include "ledger/LedgerManager.h"
#include "ledger/NetworkConfig.h"
#include "overlay/StellarXDR.h"
#include "util/types.h"
#include <medida/metrics_registry.h>
#include <memory>

namespace medida
{
class MetricsRegistry;
}

namespace stellar
{
class AbstractLedgerTxn;
class LedgerManager;
class LedgerTxnEntry;
class LedgerTxnHeader;

class SignatureChecker;
class TransactionFrame;
class MutableTransactionResultBase;

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
    OperationResult& mResult;

    virtual bool doCheckValid(SorobanNetworkConfig const& config,
                              Config const& appConfig, uint32_t ledgerVersion,
                              MutableTransactionResultBase& txResult);
    virtual bool doCheckValid(uint32_t ledgerVersion) = 0;

    virtual bool doApply(Application& app, AbstractLedgerTxn& ltx,
                         Hash const& sorobanBasePrngSeed,
                         MutableTransactionResultBase& txResult);
    virtual bool doApply(AbstractLedgerTxn& ltx,
                         MutableTransactionResultBase& txResult) = 0;

    // returns the threshold this operation requires
    virtual ThresholdLevel getThresholdLevel() const;

    // returns true if the operation is supported given a protocol version and
    // header flags
    virtual bool isOpSupported(LedgerHeader const& header) const;

    LedgerTxnEntry loadSourceAccount(AbstractLedgerTxn& ltx,
                                     LedgerTxnHeader const& header,
                                     MutableTransactionResultBase& txResult);

    // given an operation, gives a default value representing "success" for the
    // result
    void resetResultSuccess();

  public:
    static std::shared_ptr<OperationFrame>
    makeHelper(Operation const& op, OperationResult& res,
               TransactionFrame const& parentTx, uint32_t index);

    OperationFrame(Operation const& op, OperationResult& res,
                   TransactionFrame const& parentTx);
    OperationFrame(OperationFrame const&) = delete;
    virtual ~OperationFrame() = default;

    bool checkSignature(SignatureChecker& signatureChecker,
                        AbstractLedgerTxn& ltx,
                        MutableTransactionResultBase& txResult, bool forApply);

    AccountID getSourceID() const;

    OperationResult&
    getResult() const
    {
        return mResult;
    }
    OperationResultCode getResultCode() const;

    bool checkValid(Application& app, SignatureChecker& signatureChecker,
                    AbstractLedgerTxn& ltxOuter, bool forApply,
                    MutableTransactionResultBase& txResult);

    bool apply(Application& app, SignatureChecker& signatureChecker,
               AbstractLedgerTxn& ltx, Hash const& sorobanBasePrngSeed,
               MutableTransactionResultBase& txResult);

    Operation const&
    getOperation() const
    {
        return mOperation;
    }

    virtual void
    insertLedgerKeysToPrefetch(UnorderedSet<LedgerKey>& keys) const;

    virtual bool isDexOperation() const;

    virtual bool isSoroban() const;
};
}
