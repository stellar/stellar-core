// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/ExtendFootprintTTLOpFrame.h"

#include "ledger/LedgerManager.h"
#include "ledger/LedgerTypeUtils.h"
#include "ledger/NetworkConfig.h"
#include "main/AppConnector.h"
#include "transactions/MutableTransactionResult.h"
#include "transactions/TransactionUtils.h"
#include "util/GlobalChecks.h"
#include "util/ProtocolVersion.h"
#include <stdexcept>

namespace stellar
{

static ExtendFootprintTTLResult&
innerResult(OperationResult& res)
{
    return res.tr().extendFootprintTTLResult();
}

ExtendFootprintTTLOpFrame::ExtendFootprintTTLOpFrame(
    Operation const& op, TransactionFrame const& parentTx)
    : OperationFrame(op, parentTx)
    , mExtendFootprintTTLOp(mOperation.body.extendFootprintTTLOp())
{
}

bool
ExtendFootprintTTLOpFrame::isOpSupported(LedgerHeader const& header) const
{
    return protocolVersionStartsFrom(header.ledgerVersion,
                                     SOROBAN_PROTOCOL_VERSION);
}


bool
ExtendFootprintTTLOpFrame::doApplyForSoroban(
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
ExtendFootprintTTLOpFrame::doApply(AppConnector& app, AbstractLedgerTxn& ltx,
                                   OperationResult& res,
                                   OperationMetaBuilder& opMeta) const
{
    throw std::runtime_error(
        "ExtendFootprintTTLOpFrame may only be applied via doApplyForSoroban");
}

bool
ExtendFootprintTTLOpFrame::doCheckValidForSoroban(
    SorobanNetworkConfig const& networkConfig, Config const& appConfig,
    uint32_t ledgerVersion, OperationResult& res,
    DiagnosticEventManager& diagnosticEvents) const
{
    auto const& footprint = mParentTx.sorobanResources().footprint;
    if (!footprint.readWrite.empty())
    {
        innerResult(res).code(EXTEND_FOOTPRINT_TTL_MALFORMED);
        diagnosticEvents.pushError(
            SCE_STORAGE, SCEC_INVALID_INPUT,
            "read-write footprint must be empty for ExtendFootprintTTL "
            "operation",
            {});
        return false;
    }

    for (auto const& lk : footprint.readOnly)
    {
        if (!isSorobanEntry(lk))
        {
            innerResult(res).code(EXTEND_FOOTPRINT_TTL_MALFORMED);
            diagnosticEvents.pushError(
                SCE_STORAGE, SCEC_INVALID_INPUT,
                "only entries with TTL (contract data or code entries) can "
                "have it extended",
                {});
            return false;
        }
    }

    if (mExtendFootprintTTLOp.extendTo >
        networkConfig.stateArchivalSettings().maxEntryTTL - 1)
    {
        innerResult(res).code(EXTEND_FOOTPRINT_TTL_MALFORMED);
        diagnosticEvents.pushError(
            SCE_STORAGE, SCEC_INVALID_INPUT,
            "TTL extension is too large: {} > {}",
            {
                makeU64SCVal(mExtendFootprintTTLOp.extendTo),
                makeU64SCVal(networkConfig.stateArchivalSettings().maxEntryTTL -
                             1),
            });
        return false;
    }

    return true;
}

bool
ExtendFootprintTTLOpFrame::doCheckValid(uint32_t ledgerVersion,
                                        OperationResult& res) const
{
    throw std::runtime_error(
        "ExtendFootprintTTLOpFrame::doCheckValid needs Config");
}

void
ExtendFootprintTTLOpFrame::insertLedgerKeysToPrefetch(
    UnorderedSet<LedgerKey>& keys) const
{
}

bool
ExtendFootprintTTLOpFrame::isSoroban() const
{
    return true;
}

ThresholdLevel
ExtendFootprintTTLOpFrame::getThresholdLevel() const
{
    return ThresholdLevel::LOW;
}

bool
ExtendFootprintTTLOpFrame::doesAccessFrozenKey(
    SorobanNetworkConfig const& sorobanConfig) const
{
    // Soroban footprint checks happen at transaction level, so we can safely
    // say that the operation itself doesn't access frozen keys.
    return false;
}

}
