// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/RestoreFootprintOpFrame.h"
#include "TransactionUtils.h"
#include "bucket/HotArchiveBucket.h"
#include "ledger/LedgerManagerImpl.h"
#include "ledger/LedgerTypeUtils.h"
#include "medida/meter.h"
#include "medida/timer.h"
#include "transactions/MutableTransactionResult.h"
#include "transactions/ParallelApplyUtils.h"
#include "util/ProtocolVersion.h"
#include <Tracy.hpp>

namespace stellar
{

struct RestoreFootprintMetrics
{
    SorobanMetrics& mMetrics;

    uint32_t mLedgerReadByte{0};
    uint32_t mLedgerWriteByte{0};

    RestoreFootprintMetrics(SorobanMetrics& metrics) : mMetrics(metrics)
    {
    }

    ~RestoreFootprintMetrics()
    {
        mMetrics.mRestoreFpOpReadLedgerByte.Mark(mLedgerReadByte);
        mMetrics.mRestoreFpOpWriteLedgerByte.Mark(mLedgerWriteByte);
    }
    medida::TimerContext
    getExecTimer()
    {
        return mMetrics.mRestoreFpOpExec.TimeScope();
    }
};

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

ParallelTxReturnVal
RestoreFootprintOpFrame::doParallelApply(
    AppConnector& app, ThreadParallelApplyLedgerState const& threadState,
    Config const& appConfig, SorobanNetworkConfig const& sorobanConfig,
    Hash const& txPrngSeed, ParallelLedgerInfo const& ledgerInfo,
    SorobanMetrics& sorobanMetrics, OperationResult& res,
    std::optional<RefundableFeeTracker>& refundableFeeTracker,
    OperationMetaBuilder& opMeta) const
{
    ZoneNamedN(applyZone, "RestoreFootprintOpFrame apply", true);
    releaseAssertOrThrow(
        protocolVersionStartsFrom(ledgerInfo.getLedgerVersion(),
                                  PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION));
    releaseAssertOrThrow(refundableFeeTracker);

    RestoreFootprintMetrics metrics(sorobanMetrics);
    auto timeScope = metrics.getExecTimer();

    auto liveSnapshot = app.copySearchableLiveBucketListSnapshot();

    auto const& resources = mParentTx.sorobanResources();
    auto const& footprint = resources.footprint;
    auto ledgerSeq = ledgerInfo.getLedgerSeq();
    auto hotArchive = app.copySearchableHotArchiveBucketListSnapshot();

    // Keep track of LedgerEntry updates we need to make
    OpModifiedEntryMap opEntryMap;

    RestoredEntries restoredEntries;

    auto const& archivalSettings = sorobanConfig.stateArchivalSettings();
    rust::Vec<CxxLedgerEntryRentChange> rustEntryRentChanges;
    // Extend the TTL on the restored entry to minimum TTL, including
    // the current ledger.
    uint32_t restoredLiveUntilLedger =
        ledgerSeq + archivalSettings.minPersistentTTL - 1;
    rustEntryRentChanges.reserve(footprint.readWrite.size());
    auto& diagnosticEvents = opMeta.getDiagnosticEventManager();
    for (auto const& lk : footprint.readWrite)
    {
        std::shared_ptr<HotArchiveBucketEntry const> hotArchiveEntry{nullptr};
        auto ttlKey = getTTLKey(lk);
        {
            // First check the live BucketList
            auto ttlLeOpt =
                getLiveEntry(ttlKey, liveSnapshot, threadState.getEntryMap());
            if (!ttlLeOpt)
            {
                // this entry has already been restored and then deleted
                if (threadState.entryWasRestored(lk))
                {
                    continue;
                }
                hotArchiveEntry = hotArchive->load(lk);
                if (!hotArchiveEntry)
                {
                    // Entry doesn't exist, skip
                    continue;
                }
            }
            // Skip entry if it's already live.
            else if (isLive(*ttlLeOpt, ledgerSeq))
            {
                continue;
            }
        }

        // We must load the ContractCode/ContractData entry for fee purposes, as
        // restore is considered a write
        uint32_t entrySize = 0;
        LedgerEntry entry;
        if (hotArchiveEntry)
        {
            entry = hotArchiveEntry->archivedEntry();

            // Update last modified ledger seq to the current ledger seq since
            // we're rewriting this entry. ltx will update this for us, but we
            // need to process the meta before ltx has a chance for the update.
            entry.lastModifiedLedgerSeq = ledgerSeq;
            entrySize = static_cast<uint32>(xdr::xdr_size(entry));
        }
        else
        {
            auto entryLeOpt =
                getLiveEntry(lk, liveSnapshot, threadState.getEntryMap());

            // We checked for TTLEntry existence above
            releaseAssertOrThrow(entryLeOpt);

            entry = *entryLeOpt;
            entrySize = static_cast<uint32>(xdr::xdr_size(entry));
        }

        metrics.mLedgerReadByte += entrySize;
        if (resources.diskReadBytes < metrics.mLedgerReadByte)
        {
            diagnosticEvents.pushError(
                SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
                "operation byte-read resources exceeds amount specified",
                {makeU64SCVal(metrics.mLedgerReadByte),
                 makeU64SCVal(resources.diskReadBytes)});
            innerResult(res).code(RESTORE_FOOTPRINT_RESOURCE_LIMIT_EXCEEDED);
            return {false, {}};
        }

        // To maintain consistency with InvokeHostFunction, TTLEntry
        // writes come out of refundable fee, so only add entrySize
        metrics.mLedgerWriteByte += entrySize;
        if (!validateContractLedgerEntry(lk, entrySize, sorobanConfig,
                                         appConfig, mParentTx,
                                         diagnosticEvents))
        {
            innerResult(res).code(RESTORE_FOOTPRINT_RESOURCE_LIMIT_EXCEEDED);
            return {false, {}};
        }

        if (resources.writeBytes < metrics.mLedgerWriteByte)
        {
            diagnosticEvents.pushError(
                SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
                "operation byte-write resources exceeds amount specified",
                {makeU64SCVal(metrics.mLedgerWriteByte),
                 makeU64SCVal(resources.writeBytes)});
            innerResult(res).code(RESTORE_FOOTPRINT_RESOURCE_LIMIT_EXCEEDED);
            return {false, {}};
        }

        rustEntryRentChanges.emplace_back(
            createEntryRentChangeWithoutModification(
                entry, entrySize,
                /*entryLiveUntilLedger=*/std::nullopt,
                /*newLiveUntilLedger=*/restoredLiveUntilLedger,
                ledgerInfo.getLedgerVersion(), app.getConfig(), sorobanConfig));

        if (hotArchiveEntry)
        {
            opEntryMap.emplace(lk, entry);
            LedgerEntry ttlEntry =
                getTTLEntryForTTLKey(ttlKey, restoredLiveUntilLedger);
            opEntryMap.emplace(ttlKey, ttlEntry);
            restoredEntries.addHotArchiveRestore(lk, entry, ttlKey, ttlEntry);
        }
        else
        {
            auto ttlLeOpt =
                getLiveEntry(ttlKey, liveSnapshot, threadState.getEntryMap());
            releaseAssertOrThrow(ttlLeOpt);
            LedgerEntry ttlEntry = *ttlLeOpt;
            ttlEntry.data.ttl().liveUntilLedgerSeq = restoredLiveUntilLedger;
            opEntryMap.emplace(ttlKey, ttlEntry);
            // Review note: there might have been a bug here, or at least
            // possibly a deviation from the invariant that every restored entry
            // is in the opEntryMap.
            restoredEntries.addLiveBucketlistRestore(lk, entry, ttlKey,
                                                     ttlEntry);
        }
    }
    int64_t rentFee = rust_bridge::compute_rent_fee(
        app.getConfig().CURRENT_LEDGER_PROTOCOL_VERSION,
        ledgerInfo.getLedgerVersion(), rustEntryRentChanges,
        sorobanConfig.rustBridgeRentFeeConfiguration(), ledgerSeq);
    if (!refundableFeeTracker->consumeRefundableSorobanResources(
            0, rentFee, ledgerInfo.getLedgerVersion(), sorobanConfig,
            app.getConfig(), mParentTx, diagnosticEvents))
    {
        innerResult(res).code(RESTORE_FOOTPRINT_INSUFFICIENT_REFUNDABLE_FEE);
        return {false, {}};
    }
    innerResult(res).code(RESTORE_FOOTPRINT_SUCCESS);
    return {true, std::move(opEntryMap), std::move(restoredEntries)};
}

bool
RestoreFootprintOpFrame::doApply(
    AppConnector& app, AbstractLedgerTxn& ltx, Hash const& sorobanBasePrngSeed,
    OperationResult& res,
    std::optional<RefundableFeeTracker>& refundableFeeTracker,
    OperationMetaBuilder& opMeta) const
{
    ZoneNamedN(applyZone, "RestoreFootprintOpFrame apply", true);
    releaseAssertOrThrow(refundableFeeTracker);
    releaseAssertOrThrow(
        protocolVersionIsBefore(ltx.loadHeader().current().ledgerVersion,
                                PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION));

    RestoreFootprintMetrics metrics(app.getSorobanMetrics());
    auto timeScope = metrics.getExecTimer();

    auto const& resources = mParentTx.sorobanResources();
    auto const& footprint = resources.footprint;
    auto ledgerSeq = ltx.loadHeader().current().ledgerSeq;
    auto const& sorobanConfig = app.getSorobanNetworkConfigForApply();
    auto const& appConfig = app.getConfig();

    auto const& archivalSettings = sorobanConfig.stateArchivalSettings();
    rust::Vec<CxxLedgerEntryRentChange> rustEntryRentChanges;
    // Extend the TTL on the restored entry to minimum TTL, including
    // the current ledger.
    uint32_t restoredLiveUntilLedger =
        ledgerSeq + archivalSettings.minPersistentTTL - 1;
    uint32_t ledgerVersion = ltx.loadHeader().current().ledgerVersion;
    rustEntryRentChanges.reserve(footprint.readWrite.size());
    auto& diagnosticEvents = opMeta.getDiagnosticEventManager();
    for (auto const& lk : footprint.readWrite)
    {
        auto ttlKey = getTTLKey(lk);
        {
            // First check the live BucketList
            auto constTTLLtxe = ltx.loadWithoutRecord(ttlKey);
            if (!constTTLLtxe)
            {
                // Entry doesn't exist, skip
                continue;
            }
            // Skip entry if it's already live.
            else if (isLive(constTTLLtxe.current(), ledgerSeq))
            {
                continue;
            }
        }

        // We must load the ContractCode/ContractData entry for fee purposes, as
        // restore is considered a write
        auto constEntryLtxe = ltx.loadWithoutRecord(lk);
        releaseAssertOrThrow(constEntryLtxe);
        LedgerEntry constEntry = constEntryLtxe.current();
        uint32_t entrySize = static_cast<uint32>(xdr::xdr_size(constEntry));

        metrics.mLedgerReadByte += entrySize;
        if (resources.diskReadBytes < metrics.mLedgerReadByte)
        {
            diagnosticEvents.pushError(
                SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
                "operation byte-read resources exceeds amount specified",
                {makeU64SCVal(metrics.mLedgerReadByte),
                 makeU64SCVal(resources.diskReadBytes)});
            innerResult(res).code(RESTORE_FOOTPRINT_RESOURCE_LIMIT_EXCEEDED);
            return false;
        }

        // To maintain consistency with InvokeHostFunction, TTLEntry
        // writes come out of refundable fee, so only add entrySize
        metrics.mLedgerWriteByte += entrySize;
        if (!validateContractLedgerEntry(lk, entrySize, sorobanConfig,
                                         appConfig, mParentTx,
                                         diagnosticEvents))
        {
            innerResult(res).code(RESTORE_FOOTPRINT_RESOURCE_LIMIT_EXCEEDED);
            return false;
        }

        if (resources.writeBytes < metrics.mLedgerWriteByte)
        {
            diagnosticEvents.pushError(
                SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
                "operation byte-write resources exceeds amount specified",
                {makeU64SCVal(metrics.mLedgerWriteByte),
                 makeU64SCVal(resources.writeBytes)});
            innerResult(res).code(RESTORE_FOOTPRINT_RESOURCE_LIMIT_EXCEEDED);
            return false;
        }

        rustEntryRentChanges.emplace_back(
            createEntryRentChangeWithoutModification(
                constEntry, entrySize,
                /*entryLiveUntilLedger=*/std::nullopt,
                /*newLiveUntilLedger=*/restoredLiveUntilLedger, ledgerVersion,
                app.getConfig(), sorobanConfig));

        // Entry exists in the live BucketList if we get to this point due
        // to the constTTLLtxe loadWithoutRecord logic above.
        // Get the actual ledger entry (since we know it exists already at
        // this point)
        auto entry = ltx.getNewestVersion(lk);
        releaseAssertOrThrow(entry);
        ltx.restoreFromLiveBucketList(entry->ledgerEntry(),
                                      restoredLiveUntilLedger);
    }
    int64_t rentFee = rust_bridge::compute_rent_fee(
        app.getConfig().CURRENT_LEDGER_PROTOCOL_VERSION, ledgerVersion,
        rustEntryRentChanges, sorobanConfig.rustBridgeRentFeeConfiguration(),
        ledgerSeq);
    if (!refundableFeeTracker->consumeRefundableSorobanResources(
            0, rentFee, ledgerVersion, sorobanConfig, app.getConfig(),
            mParentTx, diagnosticEvents))
    {
        innerResult(res).code(RESTORE_FOOTPRINT_INSUFFICIENT_REFUNDABLE_FEE);
        return false;
    }
    innerResult(res).code(RESTORE_FOOTPRINT_SUCCESS);
    return true;
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
}
