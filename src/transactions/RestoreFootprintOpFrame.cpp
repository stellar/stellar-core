// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/RestoreFootprintOpFrame.h"

#include "ledger/LedgerManager.h"
#include "ledger/LedgerTypeUtils.h"
#include "ledger/NetworkConfig.h"
#include "main/AppConnector.h"
#include "transactions/MutableTransactionResult.h"
#include "util/GlobalChecks.h"
#include "util/ProtocolVersion.h"
#include <stdexcept>

namespace stellar
{

static RestoreFootprintResult&
innerResult(OperationResult& res)
{
    return res.tr().restoreFootprintResult();
}

RestoreFootprintOpFrame::RestoreFootprintOpFrame(
    Operation const& op, TransactionFrame const& parentTx)
    : OperationFrame(op, parentTx)
    , mRestoreFootprintOp(mOperation.body.restoreFootprintOp())
{
}

bool
RestoreFootprintOpFrame::isOpSupported(LedgerHeader const& header) const
{
    return protocolVersionStartsFrom(header.ledgerVersion,
                                     SOROBAN_PROTOCOL_VERSION);
}

bool
RestoreFootprintOpFrame::doApplyForSoroban(
    AppConnector& app, AbstractLedgerTxn& ltx,
    SorobanNetworkConfig const& sorobanConfig, Hash const& sorobanBasePrngSeed,
    OperationResult& res,
    std::optional<RefundableFeeTracker>& refundableFeeTracker,
    OperationMetaBuilder& opMeta) const
{
    // Soroban apply has fully moved to Rust (see
    // LedgerManagerImpl::applySorobanPhaseRust). The C++ op-frame apply
    // path is no longer reachable.
    releaseAssert(false);
}

bool
RestoreFootprintOpFrame::doApply(AppConnector& app, AbstractLedgerTxn& ltx,
                                 OperationResult& res,
                                 OperationMetaBuilder& opMeta) const
{
    throw std::runtime_error(
        "RestoreFootprintOpFrame may only be applied via doApplyForSoroban");
}

bool
RestoreFootprintOpFrame::doCheckValidForSoroban(
    SorobanNetworkConfig const& networkConfig, Config const& appConfig,
    uint32_t ledgerVersion, OperationResult& res,
    DiagnosticEventManager& diagnosticEvents) const
{
    auto const& footprint = mParentTx.sorobanResources().footprint;
    if (!footprint.readOnly.empty())
    {
        innerResult(res).code(RESTORE_FOOTPRINT_MALFORMED);
        diagnosticEvents.pushError(
            SCE_STORAGE, SCEC_INVALID_INPUT,
            "read-only footprint must be empty for RestoreFootprint operation",
            {});
        return false;
    }

    for (auto const& lk : footprint.readWrite)
    {
        if (!isPersistentEntry(lk))
        {
            innerResult(res).code(RESTORE_FOOTPRINT_MALFORMED);
            diagnosticEvents.pushError(
                SCE_STORAGE, SCEC_INVALID_INPUT,
                "only persistent Soroban entries can be restored", {});
            return false;
        }
    }

    return true;
}

bool
RestoreFootprintOpFrame::doCheckValid(uint32_t ledgerVersion,
                                      OperationResult& res) const
{
    throw std::runtime_error(
        "RestoreFootprintOpFrame::doCheckValid needs Config");
}

void
RestoreFootprintOpFrame::insertLedgerKeysToPrefetch(
    UnorderedSet<LedgerKey>& keys) const
{
}

bool
RestoreFootprintOpFrame::isSoroban() const
{
    return true;
}

ThresholdLevel
RestoreFootprintOpFrame::getThresholdLevel() const
{
    return ThresholdLevel::LOW;
}

bool
RestoreFootprintOpFrame::doesAccessFrozenKey(
    SorobanNetworkConfig const& sorobanConfig) const
{
    // Soroban footprint checks happen at transaction level, so we can safely
    // say that the operation itself doesn't access frozen keys.
    return false;
}
}
