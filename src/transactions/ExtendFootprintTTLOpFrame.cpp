// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/ExtendFootprintTTLOpFrame.h"
#include "TransactionUtils.h"
#include "ledger/LedgerManagerImpl.h"
#include "ledger/LedgerTypeUtils.h"
#include "medida/meter.h"
#include "medida/timer.h"
#include "transactions/MutableTransactionResult.h"
#include "util/GlobalChecks.h"
#include <Tracy.hpp>

namespace stellar
{

struct ExtendFootprintTTLMetrics
{
    SorobanMetrics& mMetrics;

    uint32 mLedgerReadByte{0};

    ExtendFootprintTTLMetrics(SorobanMetrics& metrics) : mMetrics(metrics)
    {
    }

    ~ExtendFootprintTTLMetrics()
    {
        mMetrics.mExtFpTtlOpReadLedgerByte.Mark(mLedgerReadByte);
    }
    medida::TimerContext
    getExecTimer()
    {
        return mMetrics.mExtFpTtlOpExec.TimeScope();
    }
};

ExtendFootprintTTLOpFrame::ExtendFootprintTTLOpFrame(
    Operation const& op, TransactionFrame const& parentTx)
    : OperationFrame(op, parentTx)
    , mExtendFootprintTTLOp(mOperation.body.extendFootprintTTLOp())
{
}

bool
ExtendFootprintTTLOpFrame::isOpSupported(LedgerHeader const& header) const
{
    return header.ledgerVersion >= 20;
}

bool
ExtendFootprintTTLOpFrame::doApply(
    AppConnector& app, AbstractLedgerTxn& ltx, Hash const& sorobanBasePrngSeed,
    OperationResult& res, std::shared_ptr<SorobanTxData> sorobanData) const
{
    releaseAssertOrThrow(sorobanData);
    ZoneNamedN(applyZone, "ExtendFootprintTTLOpFrame apply", true);

    ExtendFootprintTTLMetrics metrics(app.getSorobanMetrics());
    auto timeScope = metrics.getExecTimer();

    auto const& resources = mParentTx.sorobanResources();
    auto const& footprint = resources.footprint;
    auto const& sorobanConfig = app.getSorobanNetworkConfigForApply();

    rust::Vec<CxxLedgerEntryRentChange> rustEntryRentChanges;
    rustEntryRentChanges.reserve(footprint.readOnly.size());
    uint32_t ledgerSeq = ltx.loadHeader().current().ledgerSeq;
    // Extend for `extendTo` more ledgers since the current
    // ledger. Current ledger has to be payed for in order for entry
    // to be extendable, hence don't include it.
    uint32_t newLiveUntilLedgerSeq = ledgerSeq + mExtendFootprintTTLOp.extendTo;
    for (auto const& lk : footprint.readOnly)
    {
        auto ttlKey = getTTLKey(lk);
        {
            // Initially load without record since we may not need to modify
            // entry
            auto ttlConstLtxe = ltx.loadWithoutRecord(ttlKey);
            if (!ttlConstLtxe || !isLive(ttlConstLtxe.current(), ledgerSeq))
            {
                // Skip archived entries, as those must be restored.
                //
                // Also skip the missing entries. Since this happens at apply
                // time and we refund the unspent fees, it is more beneficial
                // to extend as many entries as possible.
                continue;
            }

            auto currLiveUntilLedgerSeq =
                ttlConstLtxe.current().data.ttl().liveUntilLedgerSeq;
            if (currLiveUntilLedgerSeq >= newLiveUntilLedgerSeq)
            {
                continue;
            }
        }

        // Load the ContractCode/ContractData entry for fee calculation.
        auto entryLtxe = ltx.loadWithoutRecord(lk);

        // We checked for TTLEntry existence above
        releaseAssertOrThrow(entryLtxe);

        uint32_t entrySize =
            static_cast<uint32>(xdr::xdr_size(entryLtxe.current()));
        metrics.mLedgerReadByte += entrySize;

        if (!validateContractLedgerEntry(lk, entrySize, sorobanConfig,
                                         app.getConfig(), mParentTx,
                                         *sorobanData))
        {
            innerResult(res).code(EXTEND_FOOTPRINT_TTL_RESOURCE_LIMIT_EXCEEDED);
            return false;
        }

        if (resources.readBytes < metrics.mLedgerReadByte)
        {
            sorobanData->pushApplyTimeDiagnosticError(
                app.getConfig(), SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
                "operation byte-read resources exceeds amount specified",
                {makeU64SCVal(metrics.mLedgerReadByte),
                 makeU64SCVal(resources.readBytes)});

            innerResult(res).code(EXTEND_FOOTPRINT_TTL_RESOURCE_LIMIT_EXCEEDED);
            return false;
        }

        // We already checked that the TTLEntry exists in the logic above
        auto ttlLtxe = ltx.load(ttlKey);

        rustEntryRentChanges.emplace_back();
        auto& rustChange = rustEntryRentChanges.back();
        rustChange.is_persistent = !isTemporaryEntry(lk);
        rustChange.old_size_bytes = static_cast<uint32>(entrySize);
        rustChange.new_size_bytes = rustChange.old_size_bytes;
        rustChange.old_live_until_ledger =
            ttlLtxe.current().data.ttl().liveUntilLedgerSeq;
        rustChange.new_live_until_ledger = newLiveUntilLedgerSeq;
        ttlLtxe.current().data.ttl().liveUntilLedgerSeq = newLiveUntilLedgerSeq;
    }
    uint32_t ledgerVersion = ltx.loadHeader().current().ledgerVersion;
    // This may throw, but only in case of the Core version misconfiguration.
    int64_t rentFee = rust_bridge::compute_rent_fee(
        app.getConfig().CURRENT_LEDGER_PROTOCOL_VERSION, ledgerVersion,
        rustEntryRentChanges, sorobanConfig.rustBridgeRentFeeConfiguration(),
        ledgerSeq);
    if (!sorobanData->consumeRefundableSorobanResources(
            0, rentFee, ledgerVersion, sorobanConfig, app.getConfig(),
            mParentTx))
    {
        innerResult(res).code(EXTEND_FOOTPRINT_TTL_INSUFFICIENT_REFUNDABLE_FEE);
        return false;
    }
    innerResult(res).code(EXTEND_FOOTPRINT_TTL_SUCCESS);
    return true;
}

bool
ExtendFootprintTTLOpFrame::doCheckValidForSoroban(
    SorobanNetworkConfig const& networkConfig, Config const& appConfig,
    uint32_t ledgerVersion, OperationResult& res,
    SorobanTxData& sorobanData) const
{
    auto const& footprint = mParentTx.sorobanResources().footprint;
    if (!footprint.readWrite.empty())
    {
        innerResult(res).code(EXTEND_FOOTPRINT_TTL_MALFORMED);
        sorobanData.pushValidationTimeDiagnosticError(
            appConfig, SCE_STORAGE, SCEC_INVALID_INPUT,
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
            sorobanData.pushValidationTimeDiagnosticError(
                appConfig, SCE_STORAGE, SCEC_INVALID_INPUT,
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
        sorobanData.pushValidationTimeDiagnosticError(
            appConfig, SCE_STORAGE, SCEC_INVALID_INPUT,
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

}
