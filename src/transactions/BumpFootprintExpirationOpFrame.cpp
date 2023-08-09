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
    uint32_t bumpLedger =
        ledgerSeq + mBumpFootprintExpirationOp.ledgersToExpire;
    for (auto const& lk : footprint.readOnly)
    {
        // TODO: when we move to use EXPIRATION_EXTENSIONS, this should become a
        // loadWithoutRecord, and the metrics should be updated.
        auto ltxe = ltx.load(lk);
        if (!ltxe || !isLive(ltxe.current(), ledgerSeq))
        {
            // Skip the missing entries. Since this happens at apply
            // time and we refund the unspent fees, it is more beneficial
            // to bump as many entries as possible.
            continue;
        }

        auto keySize = static_cast<uint32>(xdr::xdr_size(lk));
        auto entrySize = static_cast<uint32>(xdr::xdr_size(ltxe.current()));

        metrics.mLedgerReadByte += keySize + entrySize;
        if (resources.readBytes < metrics.mLedgerReadByte)
        {
            innerResult().code(
                BUMP_FOOTPRINT_EXPIRATION_RESOURCE_LIMIT_EXCEEDED);
            return false;
        }
        uint32_t currExpiration = getExpirationLedger(ltxe.current());

        if (currExpiration >= bumpLedger)
        {
            continue;
        }

        rustEntryRentChanges.emplace_back();
        auto& rustChange = rustEntryRentChanges.back();
        rustChange.is_persistent = !isTemporaryEntry(lk);
        rustChange.old_size_bytes = static_cast<uint32>(keySize + entrySize);
        rustChange.new_size_bytes = rustChange.old_size_bytes;
        rustChange.old_expiration_ledger = currExpiration;
        rustChange.new_expiration_ledger = bumpLedger;
        setExpirationLedger(ltxe.current(), bumpLedger);
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
        // TODO: This probably should have a more precise error code as here
        // the refundable fee limit is exceeded (and not some resource).
        innerResult().code(BUMP_FOOTPRINT_EXPIRATION_RESOURCE_LIMIT_EXCEEDED);
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
        if (!isSorobanDataEntry(lk))
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
