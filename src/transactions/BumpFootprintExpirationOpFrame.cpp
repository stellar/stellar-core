// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
#include "transactions/BumpFootprintExpirationOpFrame.h"

namespace stellar
{

struct BumpFootprintExpirationMetrics
{
    medida::MetricsRegistry& mMetrics;

    uint32 mLedgerReadByte{0};

    BumpFootprintExpirationMetrics(medida::MetricsRegistry& metrics)
        : mMetrics(metrics)
    {
    }

    ~BumpFootprintExpirationMetrics()
    {
        mMetrics
            .NewMeter({"soroban", "bump-fprint-exp-op", "read-ledger-byte"},
                      "byte")
            .Mark(mLedgerReadByte);
    }
};

BumpFootprintExpirationOpFrame::BumpFootprintExpirationOpFrame(
    Operation const& op, OperationResult& res, TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
    , mBumpFootprintExpirationOp(mOperation.body.bumpFootprintExpirationOp())
{
}

bool
BumpFootprintExpirationOpFrame::isOpSupported(LedgerHeader const& header) const
{
    return header.ledgerVersion >= 20;
}

bool
BumpFootprintExpirationOpFrame::doApply(AbstractLedgerTxn& ltx)
{
    throw std::runtime_error(
        "BumpFootprintExpirationOpFrame::doApply needs Config");
}

bool
BumpFootprintExpirationOpFrame::doApply(Application& app,
                                        AbstractLedgerTxn& ltx,
                                        Hash const& sorobanBasePrngSeed)
{
    BumpFootprintExpirationMetrics metrics(app.getMetrics());

    auto const& resources = mParentTx.sorobanResources();
    auto const& footprint = resources.footprint;

    rust::Vec<CxxLedgerEntryRentChange> rustEntryRentChanges;
    rustEntryRentChanges.reserve(footprint.readOnly.size());
    uint32_t ledgerSeq = ltx.loadHeader().current().ledgerSeq;
    // Bump for `ledgersToExpire` more ledgers since the current
    // ledger. Current ledger has to be payed for in order for entry
    // to be bump-able, hence don't include it.
    uint32_t newExpirationLedgerSeq =
        ledgerSeq + mBumpFootprintExpirationOp.ledgersToExpire;
    for (auto const& lk : footprint.readOnly)
    {
        // Load the ContractCode/ContractData entry for fee calculation.
        auto entryLtxe = ltx.loadWithoutRecord(lk);
        if (!entryLtxe)
        {
            // Skip the missing entries. Since this happens at apply
            // time and we refund the unspent fees, it is more beneficial
            // to bump as many entries as possible.
            continue;
        }

        auto expirationKey = getExpirationKey(lk);
        uint32_t entrySize = UINT32_MAX;
        uint32_t expirationSize = UINT32_MAX;
        uint32_t currExpiration = UINT32_MAX;

        {
            // Initially load without record since we may not need to modify
            // entry
            auto expirationConstLtxe = ltx.loadWithoutRecord(expirationKey);
            releaseAssert(expirationConstLtxe);
            if (!isLive(expirationConstLtxe.current(), ledgerSeq))
            {
                // Also skip expired entries, as those must be restored
                continue;
            }

            entrySize = static_cast<uint32>(xdr::xdr_size(entryLtxe.current()));
            expirationSize = static_cast<uint32>(
                xdr::xdr_size(expirationConstLtxe.current()));

            metrics.mLedgerReadByte += entrySize + expirationSize;
            if (resources.readBytes < metrics.mLedgerReadByte)
            {
                innerResult().code(
                    BUMP_FOOTPRINT_EXPIRATION_RESOURCE_LIMIT_EXCEEDED);
                return false;
            }

            currExpiration = expirationConstLtxe.current()
                                 .data.expiration()
                                 .expirationLedgerSeq;
            if (currExpiration >= newExpirationLedgerSeq)
            {
                continue;
            }
        }

        // We already checked that the expirationEntry exists in the logic above
        auto expirationLtxe = ltx.load(expirationKey);

        rustEntryRentChanges.emplace_back();
        auto& rustChange = rustEntryRentChanges.back();
        rustChange.is_persistent = !isTemporaryEntry(lk);
        rustChange.old_size_bytes = static_cast<uint32>(entrySize);
        rustChange.new_size_bytes = rustChange.old_size_bytes;
        rustChange.old_expiration_ledger = currExpiration;
        rustChange.new_expiration_ledger = newExpirationLedgerSeq;
        expirationLtxe.current().data.expiration().expirationLedgerSeq =
            newExpirationLedgerSeq;
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
        innerResult().code(
            BUMP_FOOTPRINT_EXPIRATION_INSUFFICIENT_REFUNDABLE_FEE);
        return false;
    }
    innerResult().code(BUMP_FOOTPRINT_EXPIRATION_SUCCESS);
    return true;
}

bool
BumpFootprintExpirationOpFrame::doCheckValid(SorobanNetworkConfig const& config,
                                             uint32_t ledgerVersion)
{
    auto const& footprint = mParentTx.sorobanResources().footprint;
    if (!footprint.readWrite.empty())
    {
        innerResult().code(BUMP_FOOTPRINT_EXPIRATION_MALFORMED);
        return false;
    }

    for (auto const& lk : footprint.readOnly)
    {
        if (!isSorobanEntry(lk))
        {
            innerResult().code(BUMP_FOOTPRINT_EXPIRATION_MALFORMED);
            return false;
        }
    }

    if (mBumpFootprintExpirationOp.ledgersToExpire >
        config.stateExpirationSettings().maxEntryExpiration - 1)
    {
        innerResult().code(BUMP_FOOTPRINT_EXPIRATION_MALFORMED);
        return false;
    }

    return true;
}

bool
BumpFootprintExpirationOpFrame::doCheckValid(uint32_t ledgerVersion)
{
    throw std::runtime_error(
        "BumpFootprintExpirationOpFrame::doCheckValid needs Config");
}

void
BumpFootprintExpirationOpFrame::insertLedgerKeysToPrefetch(
    UnorderedSet<LedgerKey>& keys) const
{
}

bool
BumpFootprintExpirationOpFrame::isSoroban() const
{
    return true;
}
}

#endif // ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
