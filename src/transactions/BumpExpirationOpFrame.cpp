// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
#include "transactions/BumpExpirationOpFrame.h"

namespace stellar
{

struct BumpExpirationMetrics
{
    medida::MetricsRegistry& mMetrics;

    size_t mLedgerWriteByte{0};

    BumpExpirationMetrics(medida::MetricsRegistry& metrics) : mMetrics(metrics)
    {
    }

    ~BumpExpirationMetrics()
    {
        mMetrics
            .NewMeter({"soroban", "bump-exp-op", "write-ledger-byte"}, "byte")
            .Mark(mLedgerWriteByte);
    }
};

BumpExpirationOpFrame::BumpExpirationOpFrame(Operation const& op,
                                             OperationResult& res,
                                             TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
    , mBumpExpirationOp(mOperation.body.bumpExpirationOp())
{
}

bool
BumpExpirationOpFrame::isOpSupported(LedgerHeader const& header) const
{
    return header.ledgerVersion >= 20;
}

bool
BumpExpirationOpFrame::doApply(AbstractLedgerTxn& ltx)
{
    throw std::runtime_error("BumpExpirationOpFrame::doApply needs Config");
}

bool
BumpExpirationOpFrame::doApply(Application& app, AbstractLedgerTxn& ltx,
                               Hash const& sorobanBasePrngSeed)
{
    BumpExpirationMetrics metrics(app.getMetrics());

    auto const& resources = mParentTx.sorobanResources();
    auto const& footprint = resources.footprint;

    UnorderedMap<LedgerKey, uint32_t> originalExpirations;

    for (auto const& lk : footprint.readOnly)
    {
        // When we move to use EXPIRATION_EXTENSIONS, this should become a
        // loadWithoutRecord, and the metrics should be updated.
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
                resources.readBytes < metrics.mLedgerWriteByte ||
                resources.writeBytes < metrics.mLedgerWriteByte)
            {
                innerResult().code(BUMP_EXPIRATION_RESOURCE_LIMIT_EXCEEDED);
                return false;
            }

            originalExpirations.emplace(LedgerEntryKey(ltxe.current()),
                                        getExpirationLedger(ltxe.current()));

            auto ledgerSeq = ltx.loadHeader().current().ledgerSeq;
            uint32_t bumpTo = UINT32_MAX;
            if (UINT32_MAX - ledgerSeq > mBumpExpirationOp.ledgersToExpire())
            {
                bumpTo = ledgerSeq + mBumpExpirationOp.ledgersToExpire();
            }

            if (getExpirationLedger(ltxe.current()) < bumpTo)
            {
                setExpirationLedger(ltxe.current(), bumpTo);
            }
        }
    }

    mParentTx.pushInitialExpirations(std::move(originalExpirations));

    innerResult().code(BUMP_EXPIRATION_SUCCESS);
    return true;
}

bool
BumpExpirationOpFrame::doCheckValid(SorobanNetworkConfig const& config,
                                    uint32_t ledgerVersion)
{
    auto const& footprint = mParentTx.sorobanResources().footprint;
    if (!footprint.readWrite.empty())
    {
        innerResult().code(BUMP_EXPIRATION_MALFORMED);
        return false;
    }

    for (auto const& lk : footprint.readOnly)
    {
        if (!isSorobanEntry(lk))
        {
            innerResult().code(BUMP_EXPIRATION_MALFORMED);
            return false;
        }
    }

    if (mBumpExpirationOp.ledgersToExpire() >
        config.stateExpirationSettings().maxEntryExpiration)
    {
        innerResult().code(BUMP_EXPIRATION_MALFORMED);
        return false;
    }

    return true;
}

bool
BumpExpirationOpFrame::doCheckValid(uint32_t ledgerVersion)
{
    throw std::runtime_error(
        "BumpExpirationOpFrame::doCheckValid needs Config");
}

void
BumpExpirationOpFrame::insertLedgerKeysToPrefetch(
    UnorderedSet<LedgerKey>& keys) const
{
}

bool
BumpExpirationOpFrame::isSoroban() const
{
    return true;
}
}

#endif // ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
