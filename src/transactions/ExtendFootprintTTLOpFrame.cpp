// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/ExtendFootprintTTLOpFrame.h"
#include "TransactionUtils.h"

namespace stellar
{

struct ExtendFootprintTTLMetrics
{
    medida::MetricsRegistry& mMetrics;

    uint32 mLedgerReadByte{0};

    ExtendFootprintTTLMetrics(medida::MetricsRegistry& metrics)
        : mMetrics(metrics)
    {
    }

    ~ExtendFootprintTTLMetrics()
    {
        mMetrics
            .NewMeter({"soroban", "ext-fprint-ttl-op", "read-ledger-byte"},
                      "byte")
            .Mark(mLedgerReadByte);
    }
};

ExtendFootprintTTLOpFrame::ExtendFootprintTTLOpFrame(Operation const& op,
                                                     OperationResult& res,
                                                     TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
    , mExtendFootprintTTLOp(mOperation.body.extendFootprintTTLOp())
{
}

bool
ExtendFootprintTTLOpFrame::isOpSupported(LedgerHeader const& header) const
{
    return header.ledgerVersion >= 20;
}

bool
ExtendFootprintTTLOpFrame::doApply(AbstractLedgerTxn& ltx)
{
    throw std::runtime_error("ExtendFootprintTTLOpFrame::doApply needs Config");
}

bool
ExtendFootprintTTLOpFrame::doApply(Application& app, AbstractLedgerTxn& ltx,
                                   Hash const& sorobanBasePrngSeed)
{
    ExtendFootprintTTLMetrics metrics(app.getMetrics());

    auto const& resources = mParentTx.sorobanResources();
    auto const& footprint = resources.footprint;

    rust::Vec<CxxLedgerEntryRentChange> rustEntryRentChanges;
    rustEntryRentChanges.reserve(footprint.readOnly.size());
    uint32_t ledgerSeq = ltx.loadHeader().current().ledgerSeq;
    // Extend for `extendTo` more ledgers since the current
    // ledger. Current ledger has to be payed for in order for entry
    // to be extendable, hence don't include it.
    uint32_t newLiveUntilLedgerSeq = ledgerSeq + mExtendFootprintTTLOp.extendTo;
    for (auto const& lk : footprint.readOnly)
    {
        // Load the ContractCode/ContractData entry for fee calculation.
        auto entryLtxe = ltx.loadWithoutRecord(lk);
        if (!entryLtxe)
        {
            // Skip the missing entries. Since this happens at apply
            // time and we refund the unspent fees, it is more beneficial
            // to extend as many entries as possible.
            continue;
        }

        auto ttlKey = getTTLKey(lk);
        uint32_t entrySize = UINT32_MAX;
        uint32_t ttlSize = UINT32_MAX;
        uint32_t currLiveUntilLedgerSeq = UINT32_MAX;

        {
            // Initially load without record since we may not need to modify
            // entry
            auto ttlConstLtxe = ltx.loadWithoutRecord(ttlKey);
            releaseAssertOrThrow(ttlConstLtxe);
            if (!isLive(ttlConstLtxe.current(), ledgerSeq))
            {
                // Also skip archived entries, as those must be restored
                continue;
            }

            entrySize = static_cast<uint32>(xdr::xdr_size(entryLtxe.current()));
            ttlSize =
                static_cast<uint32>(xdr::xdr_size(ttlConstLtxe.current()));

            metrics.mLedgerReadByte += entrySize + ttlSize;
            if (resources.readBytes < metrics.mLedgerReadByte)
            {
                mParentTx.pushSimpleDiagnosticError(
                    SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
                    "operation byte-read resources exceeds amount specified",
                    {makeU64SCVal(metrics.mLedgerReadByte),
                     makeU64SCVal(resources.readBytes)});

                innerResult().code(
                    EXTEND_FOOTPRINT_TTL_RESOURCE_LIMIT_EXCEEDED);
                return false;
            }

            currLiveUntilLedgerSeq =
                ttlConstLtxe.current().data.ttl().liveUntilLedgerSeq;
            if (currLiveUntilLedgerSeq >= newLiveUntilLedgerSeq)
            {
                continue;
            }
        }

        // We already checked that the TTLEntry exists in the logic above
        auto ttlLtxe = ltx.load(ttlKey);

        rustEntryRentChanges.emplace_back();
        auto& rustChange = rustEntryRentChanges.back();
        rustChange.is_persistent = !isTemporaryEntry(lk);
        rustChange.old_size_bytes = static_cast<uint32>(entrySize);
        rustChange.new_size_bytes = rustChange.old_size_bytes;
        rustChange.old_live_until_ledger = currLiveUntilLedgerSeq;
        rustChange.new_live_until_ledger = newLiveUntilLedgerSeq;
        ttlLtxe.current().data.ttl().liveUntilLedgerSeq = newLiveUntilLedgerSeq;
    }
    uint32_t ledgerVersion = ltx.loadHeader().current().ledgerVersion;
    // This may throw, but only in case of the Core version misconfiguration.
    int64_t rentFee = rust_bridge::compute_rent_fee(
        app.getConfig().CURRENT_LEDGER_PROTOCOL_VERSION, ledgerVersion,
        rustEntryRentChanges,
        app.getLedgerManager()
            .getSorobanNetworkConfig(ltx)
            .rustBridgeRentFeeConfiguration(),
        ledgerSeq);
    if (!mParentTx.consumeRefundableSorobanResources(
            0, rentFee, ledgerVersion,
            app.getLedgerManager().getSorobanNetworkConfig(ltx),
            app.getConfig()))
    {
        innerResult().code(EXTEND_FOOTPRINT_TTL_INSUFFICIENT_REFUNDABLE_FEE);
        return false;
    }
    innerResult().code(EXTEND_FOOTPRINT_TTL_SUCCESS);
    return true;
}

bool
ExtendFootprintTTLOpFrame::doCheckValid(SorobanNetworkConfig const& config,
                                        uint32_t ledgerVersion)
{
    auto const& footprint = mParentTx.sorobanResources().footprint;
    if (!footprint.readWrite.empty())
    {
        innerResult().code(EXTEND_FOOTPRINT_TTL_MALFORMED);
        return false;
    }

    for (auto const& lk : footprint.readOnly)
    {
        if (!isSorobanEntry(lk))
        {
            innerResult().code(EXTEND_FOOTPRINT_TTL_MALFORMED);
            return false;
        }
    }

    if (mExtendFootprintTTLOp.extendTo >
        config.stateArchivalSettings().maxEntryTTL - 1)
    {
        innerResult().code(EXTEND_FOOTPRINT_TTL_MALFORMED);
        return false;
    }

    return true;
}

bool
ExtendFootprintTTLOpFrame::doCheckValid(uint32_t ledgerVersion)
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
}