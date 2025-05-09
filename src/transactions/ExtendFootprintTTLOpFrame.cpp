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
ExtendFootprintTTLOpFrame::doPreloadEntriesForParallelApply(
    AppConnector& app, SorobanMetrics& sorobanMetrics, AbstractLedgerTxn& ltx,
    ThreadEntryMap& entryMap, OperationResult& res,
    DiagnosticEventBuffer& buffer) const
{
    ExtendFootprintTTLMetrics metrics(sorobanMetrics);

    uint32_t ledgerSeq = ltx.loadHeader().current().ledgerSeq;
    uint32_t newLiveUntilLedgerSeq = ledgerSeq + mExtendFootprintTTLOp.extendTo;

    for (auto const& lk : mParentTx.sorobanResources().footprint.readOnly)
    {
        auto ttlKey = getTTLKey(lk);
        {
            auto ttlConstLtxe = ltx.loadWithoutRecord(ttlKey);
            if (!ttlConstLtxe)
            {
                entryMap.emplace(lk, ThreadEntry{std::nullopt, false});
                entryMap.emplace(ttlKey, ThreadEntry{std::nullopt, false});
                // Skip archived and missing entries
                continue;
            }

            entryMap.emplace(ttlKey,
                             ThreadEntry{ttlConstLtxe.current(), false});

            if (!isLive(ttlConstLtxe.current(), ledgerSeq))
            {
                // We aren't adding the entry key if it isn't live. This means
                // we will not try to access the entry after this point for this
                // transaction.
                continue;
            }

            // Skip entries that don't need to be extended
            auto currLiveUntilLedgerSeq =
                ttlConstLtxe.current().data.ttl().liveUntilLedgerSeq;
            if (currLiveUntilLedgerSeq >= newLiveUntilLedgerSeq)
            {
                continue;
            }
        }

        auto entryLtxe = ltx.loadWithoutRecord(lk);
        // We checked for TTLEntry existence above
        releaseAssertOrThrow(entryLtxe);

        uint32_t entrySize =
            static_cast<uint32>(xdr::xdr_size(entryLtxe.current()));
        metrics.mLedgerReadByte += entrySize;

        auto const& resources = mParentTx.sorobanResources();
        if (resources.diskReadBytes < metrics.mLedgerReadByte)
        {
            innerResult(res).code(EXTEND_FOOTPRINT_TTL_RESOURCE_LIMIT_EXCEEDED);

            buffer.pushApplyTimeDiagnosticError(
                SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
                "operation byte-read mresources exceeds amount specified",
                {makeU64SCVal(metrics.mLedgerReadByte),
                 makeU64SCVal(resources.diskReadBytes)});

            return false;
        }

        entryMap.emplace(lk, ThreadEntry{entryLtxe.current(), false});
    }
    return true;
}

ParallelTxReturnVal
ExtendFootprintTTLOpFrame::doParallelApply(
    AppConnector& app,
    ThreadEntryMap const& entryMap, // Must not be shared between threads
    Config const& appConfig, SorobanNetworkConfig const& sorobanConfig,
    Hash const& txPrngSeed, ParallelLedgerInfo const& ledgerInfo,
    SorobanMetrics& sorobanMetrics, OperationResult& res,
    SorobanTxData& sorobanData, OpEventManager& opEventManager) const
{
    ZoneNamedN(applyZone, "ExtendFootprintTTLOpFrame doParallelApply", true);

    // We don't use ExtendFootprintTTLMetrics here because it only tracks
    // ledgerReadBytes, which is handled in doPreloadEntriesForParallelApply
    auto timescope = sorobanMetrics.mExtFpTtlOpExec.TimeScope();

    auto const& resources = mParentTx.sorobanResources();
    auto const& footprint = resources.footprint;

    // Keep track of LedgerEntry updates we need to make
    ModifiedEntryMap opEntryMap;

    rust::Vec<CxxLedgerEntryRentChange> rustEntryRentChanges;
    rustEntryRentChanges.reserve(footprint.readOnly.size());
    // Extend for `extendTo` more ledgers since the current
    // ledger. Current ledger has to be payed for in order for entry
    // to be extendable, hence don't include it.
    uint32_t newLiveUntilLedgerSeq =
        ledgerInfo.getLedgerSeq() + mExtendFootprintTTLOp.extendTo;
    auto& diagnosticEvents = opEventManager.getDiagnosticEventsBuffer();
    for (auto const& lk : footprint.readOnly)
    {
        auto ttlKey = getTTLKey(lk);
        auto ttlIter = entryMap.find(ttlKey);

        if (ttlIter == entryMap.end() || !ttlIter->second.mLedgerEntry ||
            !isLive(*ttlIter->second.mLedgerEntry, ledgerInfo.getLedgerSeq()))
        {
            // Skip archived entries, as those must be restored.
            //
            // Also skip the missing entries. Since this happens at apply
            // time and we refund the unspent fees, it is more beneficial
            // to extend as many entries as possible.
            continue;
        }

        auto currLiveUntilLedgerSeq =
            ttlIter->second.mLedgerEntry->data.ttl().liveUntilLedgerSeq;
        if (currLiveUntilLedgerSeq >= newLiveUntilLedgerSeq)
        {
            continue;
        }

        auto entryIter = entryMap.find(lk);

        // Load the ContractCode/ContractData entry for fee calculation.

        // We checked for TTLEntry existence above
        releaseAssertOrThrow(entryIter != entryMap.end() &&
                             entryIter->second.mLedgerEntry);
        auto const& entryLe = *entryIter->second.mLedgerEntry;

        uint32_t entrySize = static_cast<uint32>(xdr::xdr_size(entryLe));

        if (!validateContractLedgerEntry(lk, entrySize, sorobanConfig,
                                         appConfig, mParentTx,
                                         diagnosticEvents))
        {
            innerResult(res).code(EXTEND_FOOTPRINT_TTL_RESOURCE_LIMIT_EXCEEDED);
            return {false, {}};
        }

        auto ttlLe = *ttlIter->second.mLedgerEntry;

        rustEntryRentChanges.emplace_back();
        auto& rustChange = rustEntryRentChanges.back();
        rustChange.is_persistent = !isTemporaryEntry(lk);

        uint32_t entrySizeForRent = entrySize;
        if (protocolVersionStartsFrom(ledgerInfo.getLedgerVersion(),
                                      ProtocolVersion::V_23))
        {
            if (isContractCodeEntry(lk))
            {
                entrySizeForRent =
                    rust_bridge::contract_code_memory_size_for_rent(
                        app.getConfig().CURRENT_LEDGER_PROTOCOL_VERSION,
                        ledgerInfo.getLedgerVersion(),
                        toCxxBuf(entryLe.data.contractCode()),
                        toCxxBuf(sorobanConfig.cpuCostParams()),
                        toCxxBuf(sorobanConfig.memCostParams()));
            }
        }

        rustChange.old_size_bytes = entrySizeForRent;

        rustChange.new_size_bytes = rustChange.old_size_bytes;
        rustChange.old_live_until_ledger = ttlLe.data.ttl().liveUntilLedgerSeq;
        rustChange.new_live_until_ledger = newLiveUntilLedgerSeq;
        ttlLe.data.ttl().liveUntilLedgerSeq = newLiveUntilLedgerSeq;

        opEntryMap.emplace(ttlKey, ttlLe);
    }

    // This may throw, but only in case of the Core version misconfiguration.
    int64_t rentFee = rust_bridge::compute_rent_fee(
        appConfig.CURRENT_LEDGER_PROTOCOL_VERSION,
        ledgerInfo.getLedgerVersion(), rustEntryRentChanges,
        sorobanConfig.rustBridgeRentFeeConfiguration(),
        ledgerInfo.getLedgerSeq());
    if (!sorobanData.consumeRefundableSorobanResources(
            0, rentFee, ledgerInfo.getLedgerVersion(), sorobanConfig, appConfig,
            mParentTx, diagnosticEvents))
    {
        innerResult(res).code(EXTEND_FOOTPRINT_TTL_INSUFFICIENT_REFUNDABLE_FEE);
        return {false, {}};
    }
    innerResult(res).code(EXTEND_FOOTPRINT_TTL_SUCCESS);
    return {true, std::move(opEntryMap)};
}

bool
ExtendFootprintTTLOpFrame::doApply(AppConnector& app, AbstractLedgerTxn& ltx,
                                   Hash const& sorobanBasePrngSeed,
                                   OperationResult& res,
                                   std::shared_ptr<SorobanTxData> sorobanData,
                                   OpEventManager& opEventManager) const
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
    uint32_t ledgerVersion = ltx.loadHeader().current().ledgerVersion;
    // Extend for `extendTo` more ledgers since the current
    // ledger. Current ledger has to be payed for in order for entry
    // to be extendable, hence don't include it.
    uint32_t newLiveUntilLedgerSeq = ledgerSeq + mExtendFootprintTTLOp.extendTo;
    auto& diagnosticEvents = opEventManager.getDiagnosticEventsBuffer();
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
                                         diagnosticEvents))
        {
            innerResult(res).code(EXTEND_FOOTPRINT_TTL_RESOURCE_LIMIT_EXCEEDED);
            return false;
        }

        if (resources.diskReadBytes < metrics.mLedgerReadByte)
        {
            diagnosticEvents.pushApplyTimeDiagnosticError(
                SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
                "operation byte-read resources exceeds amount specified",
                {makeU64SCVal(metrics.mLedgerReadByte),
                 makeU64SCVal(resources.diskReadBytes)});

            innerResult(res).code(EXTEND_FOOTPRINT_TTL_RESOURCE_LIMIT_EXCEEDED);
            return false;
        }

        // We already checked that the TTLEntry exists in the logic above
        auto ttlLtxe = ltx.load(ttlKey);

        rustEntryRentChanges.emplace_back();
        auto& rustChange = rustEntryRentChanges.back();
        rustChange.is_persistent = !isTemporaryEntry(lk);

        uint32_t entrySizeForRent = entrySize;
        if (protocolVersionStartsFrom(ledgerVersion, ProtocolVersion::V_23))
        {
            if (isContractCodeEntry(lk))
            {
                entrySizeForRent =
                    rust_bridge::contract_code_memory_size_for_rent(
                        app.getConfig().CURRENT_LEDGER_PROTOCOL_VERSION,
                        ledgerVersion,
                        toCxxBuf(entryLtxe.current().data.contractCode()),
                        toCxxBuf(sorobanConfig.cpuCostParams()),
                        toCxxBuf(sorobanConfig.memCostParams()));
            }
        }

        rustChange.old_size_bytes = entrySizeForRent;
        rustChange.new_size_bytes = rustChange.old_size_bytes;
        rustChange.old_live_until_ledger =
            ttlLtxe.current().data.ttl().liveUntilLedgerSeq;
        rustChange.new_live_until_ledger = newLiveUntilLedgerSeq;
        ttlLtxe.current().data.ttl().liveUntilLedgerSeq = newLiveUntilLedgerSeq;
    }

    // This may throw, but only in case of the Core version misconfiguration.
    int64_t rentFee = rust_bridge::compute_rent_fee(
        app.getConfig().CURRENT_LEDGER_PROTOCOL_VERSION, ledgerVersion,
        rustEntryRentChanges, sorobanConfig.rustBridgeRentFeeConfiguration(),
        ledgerSeq);
    if (!sorobanData->consumeRefundableSorobanResources(
            0, rentFee, ledgerVersion, sorobanConfig, app.getConfig(),
            mParentTx, diagnosticEvents))
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
    DiagnosticEventBuffer* diagnosticEvents) const
{
    auto const& footprint = mParentTx.sorobanResources().footprint;
    if (!footprint.readWrite.empty())
    {
        innerResult(res).code(EXTEND_FOOTPRINT_TTL_MALFORMED);
        pushValidationTimeDiagnosticError(
            diagnosticEvents, SCE_STORAGE, SCEC_INVALID_INPUT,
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
            pushValidationTimeDiagnosticError(
                diagnosticEvents, SCE_STORAGE, SCEC_INVALID_INPUT,
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
        pushValidationTimeDiagnosticError(
            diagnosticEvents, SCE_STORAGE, SCEC_INVALID_INPUT,
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
