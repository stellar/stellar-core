// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
#include "transactions/RestoreFootprintOpFrame.h"

namespace stellar
{

struct RestoreFootprintMetrics
{
    medida::MetricsRegistry& mMetrics;

    size_t mLedgerWriteByte{0};

    RestoreFootprintMetrics(medida::MetricsRegistry& metrics)
        : mMetrics(metrics)
    {
    }

    ~RestoreFootprintMetrics()
    {
        mMetrics
            .NewMeter({"soroban", "restore-fprint-op", "write-ledger-byte"},
                      "byte")
            .Mark(mLedgerWriteByte);
    }
};

RestoreFootprintOpFrame::RestoreFootprintOpFrame(Operation const& op,
                                                 OperationResult& res,
                                                 TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
    , mRestoreFootprintOp(mOperation.body.restoreFootprintOp())
{
}

bool
RestoreFootprintOpFrame::isOpSupported(LedgerHeader const& header) const
{
    return header.ledgerVersion >= 20;
}

bool
RestoreFootprintOpFrame::doApply(AbstractLedgerTxn& ltx)
{
    throw std::runtime_error("RestoreFootprintOpFrame::doApply needs Config");
}

bool
RestoreFootprintOpFrame::doApply(Application& app, AbstractLedgerTxn& ltx,
                                 Hash const& sorobanBasePrngSeed)
{
    RestoreFootprintMetrics metrics(app.getMetrics());

    auto const& resources = mParentTx.sorobanResources();
    auto const& footprint = resources.footprint;

    UnorderedMap<LedgerKey, uint32_t> originalExpirations;

    for (auto const& lk : footprint.readWrite)
    {
        auto ltxe = ltx.load(lk);
        if (ltxe)
        {
            auto keySize = xdr::xdr_size(lk);
            auto entrySize = xdr::xdr_size(ltxe.current());
            metrics.mLedgerWriteByte += keySize;
            metrics.mLedgerWriteByte += entrySize;

            // Divide the limit by 2 for the metadata check since the previous
            // state is also included in the meta.
            if (resources.extendedMetaDataSizeBytes / 2 <
                    metrics.mLedgerWriteByte ||
                resources.writeBytes < metrics.mLedgerWriteByte ||
                resources.readBytes < metrics.mLedgerWriteByte)
            {
                innerResult().code(RESTORE_FOOTPRINT_RESOURCE_LIMIT_EXCEEDED);
                return false;
            }

            originalExpirations.emplace(LedgerEntryKey(ltxe.current()),
                                        getExpirationLedger(ltxe.current()));

            auto ledgerSeq = ltx.loadHeader().current().ledgerSeq;
            auto const& expirationSettings = app.getLedgerManager()
                                                 .getSorobanNetworkConfig(ltx)
                                                 .stateExpirationSettings();
            auto minPersistentExpirationLedger =
                ledgerSeq + expirationSettings.minPersistentEntryExpiration;

            if (getExpirationLedger(ltxe.current()) <
                minPersistentExpirationLedger)
            {
                setExpirationLedger(ltxe.current(),
                                    minPersistentExpirationLedger);
            }
        }
    }

    mParentTx.pushInitialExpirations(std::move(originalExpirations));

    innerResult().code(RESTORE_FOOTPRINT_SUCCESS);
    return true;
}

bool
RestoreFootprintOpFrame::doCheckValid(SorobanNetworkConfig const& config,
                                      uint32_t ledgerVersion)
{
    auto const& footprint = mParentTx.sorobanResources().footprint;
    if (!footprint.readOnly.empty())
    {
        innerResult().code(RESTORE_FOOTPRINT_MALFORMED);
        return false;
    }

    for (auto const& lk : footprint.readWrite)
    {
        if (!isSorobanDataEntry(lk) || isTemporaryEntry(lk))
        {
            innerResult().code(RESTORE_FOOTPRINT_MALFORMED);
            return false;
        }
    }

    return true;
}

bool
RestoreFootprintOpFrame::doCheckValid(uint32_t ledgerVersion)
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
}

#endif // ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
