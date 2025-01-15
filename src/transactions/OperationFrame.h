#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerHashUtils.h"
#include "ledger/LedgerManager.h"
#include "ledger/NetworkConfig.h"
#include "main/AppConnector.h"
#include "overlay/StellarXDR.h"
#include "transactions/MutableTransactionResult.h"
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
                           SorobanTxData& sorobanData) const;
    virtual bool doCheckValid(uint32_t ledgerVersion,
                              OperationResult& res) const = 0;
    virtual bool doApply(AppConnector& app, AbstractLedgerTxn& ltx,
                         Hash const& sorobanBasePrngSeed, OperationResult& res,
                         std::shared_ptr<SorobanTxData> sorobanData) const = 0;

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

    bool checkSignature(SignatureChecker& signatureChecker,
                        LedgerSnapshot const& ls, OperationResult& res,
                        bool forApply) const;

    AccountID getSourceID() const;

    bool checkValid(AppConnector& app, SignatureChecker& signatureChecker,
                    std::optional<SorobanNetworkConfig> const& cfg,
                    LedgerSnapshot const& ls, bool forApply,
                    OperationResult& res,
                    std::shared_ptr<SorobanTxData> sorobanData) const;

    bool apply(AppConnector& app, SignatureChecker& signatureChecker,
               AbstractLedgerTxn& ltx, Hash const& sorobanBasePrngSeed,
               OperationResult& res,
               std::shared_ptr<SorobanTxData> sorobanData) const;

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
};
}
