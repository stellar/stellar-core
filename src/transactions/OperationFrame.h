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
class TransactionResultPayload;

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
    TransactionFrame& mParentTx;
    OperationResult& mResult;

    virtual bool doCheckValid(SorobanNetworkConfig const& config,
                              Config const& appConfig, uint32_t ledgerVersion,
                              TransactionResultPayload& resPayload);
    virtual bool doCheckValid(uint32_t ledgerVersion) = 0;

    virtual bool doApply(Application& app, AbstractLedgerTxn& ltx,
                         Hash const& sorobanBasePrngSeed,
                         TransactionResultPayload& resPayload);
    virtual bool doApply(AbstractLedgerTxn& ltx,
                         TransactionResultPayload& resPayload) = 0;

    // returns the threshold this operation requires
    virtual ThresholdLevel getThresholdLevel() const;

    // returns true if the operation is supported given a protocol version and
    // header flags
    virtual bool isOpSupported(LedgerHeader const& header) const;

    LedgerTxnEntry loadSourceAccount(AbstractLedgerTxn& ltx,
                                     LedgerTxnHeader const& header,
                                     TransactionResultPayload& resPayload);

    // given an operation, gives a default value representing "success" for the
    // result
    void resetResultSuccess();

  public:
    static std::shared_ptr<OperationFrame>
    makeHelper(Operation const& op, OperationResult& res,
               TransactionFrame& parentTx, uint32_t index);

    OperationFrame(Operation const& op, OperationResult& res,
                   TransactionFrame& parentTx);
    OperationFrame(OperationFrame const&) = delete;
    virtual ~OperationFrame() = default;

    bool checkSignature(SignatureChecker& signatureChecker,
                        AbstractLedgerTxn& ltx,
                        TransactionResultPayload& resPayload, bool forApply);

    AccountID getSourceID() const;

    OperationResult&
    getResult() const
    {
        return mResult;
    }
    OperationResultCode getResultCode() const;

    bool checkValid(Application& app, SignatureChecker& signatureChecker,
                    AbstractLedgerTxn& ltxOuter, bool forApply,
                    TransactionResultPayload& resPayload);

    bool apply(Application& app, SignatureChecker& signatureChecker,
               AbstractLedgerTxn& ltx, Hash const& sorobanBasePrngSeed,
               TransactionResultPayload& resPayload);

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
