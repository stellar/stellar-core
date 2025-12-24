// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/ConservationOfLumens.h"
#include "invariant/InvariantManager.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "main/Application.h"
#include "transactions/TransactionUtils.h"
#include "util/LogSlowExecution.h"
#include <numeric>

namespace stellar
{

static std::optional<int64_t>
calculateDeltaBalance(AssetContractInfo const& lumenContractInfo,
                      LedgerEntry const* current, LedgerEntry const* previous)
{
    releaseAssert(current || previous);
    auto currentBalance =
        current ? getAssetBalance(*current, Asset(ASSET_TYPE_NATIVE),
                                  lumenContractInfo)
                : AssetBalanceResult{false, true, 0};
    auto previousBalance =
        previous ? getAssetBalance(*previous, Asset(ASSET_TYPE_NATIVE),
                                   lumenContractInfo)
                 : AssetBalanceResult{false, true, 0};

    if (currentBalance.overflowed || previousBalance.overflowed)
    {
        // Overflow detected. Fail the invariant.
        return std::nullopt;
    }

    return (currentBalance.assetMatched && currentBalance.balance
                ? *currentBalance.balance
                : 0) -
           (previousBalance.assetMatched && previousBalance.balance
                ? *previousBalance.balance
                : 0);
}

static std::optional<int64_t>
calculateDeltaBalance(
    AssetContractInfo const& lumenContractInfo,
    std::shared_ptr<InternalLedgerEntry const> const& genCurrent,
    std::shared_ptr<InternalLedgerEntry const> const& genPrevious)
{
    auto type = genCurrent ? genCurrent->type() : genPrevious->type();
    if (type == InternalLedgerEntryType::LEDGER_ENTRY)
    {
        auto const* current = genCurrent ? &genCurrent->ledgerEntry() : nullptr;
        auto const* previous =
            genPrevious ? &genPrevious->ledgerEntry() : nullptr;

        return calculateDeltaBalance(lumenContractInfo, current, previous);
    }
    return 0;
}

ConservationOfLumens::ConservationOfLumens(
    AssetContractInfo const& lumenContractInfo)
    : Invariant(false), mLumenContractInfo(lumenContractInfo)
{
}

std::shared_ptr<Invariant>
ConservationOfLumens::registerInvariant(Application& app)
{
    Asset native(ASSET_TYPE_NATIVE);
    // We need to keep track of lumens in the Stellar Asset Contract, so
    // calculate the lumen contractID, the key of the Balance entry, and the
    // amount field within that entry.
    auto lumenInfo = getAssetContractInfo(native, app.getNetworkID());

    return app.getInvariantManager().registerInvariant<ConservationOfLumens>(
        lumenInfo);
}

std::string
ConservationOfLumens::getName() const
{
    return "ConservationOfLumens";
}

std::string
ConservationOfLumens::checkOnOperationApply(
    Operation const& operation, OperationResult const& result,
    LedgerTxnDelta const& ltxDelta, std::vector<ContractEvent> const& events,
    AppConnector&)
{
    auto const& lhCurr = ltxDelta.header.current;
    auto const& lhPrev = ltxDelta.header.previous;

    int64_t deltaTotalCoins = lhCurr.totalCoins - lhPrev.totalCoins;
    int64_t deltaFeePool = lhCurr.feePool - lhPrev.feePool;

    int64_t deltaBalances = 0;
    for (auto const& entryPair : ltxDelta.entry)
    {
        auto const& entryDelta = entryPair.second;
        auto delta = stellar::calculateDeltaBalance(
            mLumenContractInfo, entryDelta.current, entryDelta.previous);
        if (!delta)
        {
            return "Could not calculate lumen balance delta for an entry";
        }

        // Check for overflow and underflow
        if (*delta > 0 &&
            deltaBalances > std::numeric_limits<int64_t>::max() - *delta)
        {
            return "Overflow detected when adding to deltaBalances";
        }
        if (*delta < 0 &&
            deltaBalances < std::numeric_limits<int64_t>::min() - *delta)
        {
            return "Underflow detected when adding to deltaBalances";
        }

        deltaBalances += *delta;
    }

    if (result.tr().type() == INFLATION)
    {
        int64_t inflationPayouts =
            std::accumulate(result.tr().inflationResult().payouts().begin(),
                            result.tr().inflationResult().payouts().end(),
                            static_cast<int64_t>(0),
                            [](int64_t lhs, InflationPayout const& rhs) {
                                return lhs + rhs.amount;
                            });
        if (deltaTotalCoins != inflationPayouts + deltaFeePool)
        {
            return fmt::format(
                FMT_STRING(
                    "LedgerHeader totalCoins change ({:d}) did not match"
                    " feePool change ({:d}) plus inflation payouts ({:d})"),
                deltaTotalCoins, deltaFeePool, inflationPayouts);
        }
        if (deltaBalances != inflationPayouts)
        {
            return fmt::format(
                FMT_STRING("LedgerEntry account balances change ({:d}) "
                           "did not match inflation payouts ({:d})"),
                deltaBalances, inflationPayouts);
        }
    }
    else
    {
        if (deltaTotalCoins != 0)
        {
            return fmt::format(
                FMT_STRING("LedgerHeader totalCoins changed from {:d} to"
                           " {:d} without inflation"),
                lhPrev.totalCoins, lhCurr.totalCoins);
        }
        if (deltaFeePool != 0)
        {
            return fmt::format(
                FMT_STRING("LedgerHeader feePool changed from {:d} to"
                           " {:d} without inflation"),
                lhPrev.feePool, lhCurr.feePool);
        }
        if (deltaBalances != 0)
        {
            return fmt::format(
                FMT_STRING("LedgerEntry account balances changed by"
                           " {:d} without inflation"),
                deltaBalances);
        }
    }
    return {};
}

// Helper function that processes an entry if it hasn't been seen before.
// Returns true on success, false on error (with error set in errorMsg).
static bool
processEntryIfNew(LedgerEntry const& entry, LedgerKey const& key,
                  std::unordered_set<LedgerKey>& countedKeys,
                  Asset const& asset,
                  AssetContractInfo const& assetContractInfo,
                  int64_t& sumBalance, std::string& errorMsg)
{
    if (countedKeys.count(key) != 0)
    {
        return true;
    }

    auto result = getAssetBalance(entry, asset, assetContractInfo);

    if (result.overflowed)
    {
        errorMsg = fmt::format(
            FMT_STRING(
                "ConservationOfLumens: getAssetBalance overflow for key: {}"),
            xdrToCerealString(key, "ledger_key"));
        return false;
    }

    if (!result.assetMatched)
    {
        return true; // Asset doesn't match, skip
    }

    if (!result.balance || !addBalance(sumBalance, *result.balance))
    {
        errorMsg = fmt::format(
            FMT_STRING(
                "ConservationOfLumens: Overflow adding balance for key: {}"),
            xdrToCerealString(key, "ledger_key"));
        return false;
    }

    countedKeys.emplace(key);

    return true;
}

// Scan live bucket list for entries that can hold the native asset
static void
scanLiveBuckets(
    std::shared_ptr<SearchableLiveBucketListSnapshot const> const& liveSnapshot,
    Asset const& asset, AssetContractInfo const& assetContractInfo,
    int64_t& sumBalance, std::string& errorMsg,
    std::function<bool()> const& isStopping)
{
    // Scan all entry types that can hold the native asset
    for (auto let : xdr::xdr_traits<LedgerEntryType>::enum_values())
    {
        LedgerEntryType type = static_cast<LedgerEntryType>(let);
        if (!canHoldAsset(type, asset))
        {
            continue;
        }

        std::unordered_set<LedgerKey> countedKeys;

        liveSnapshot->scanForEntriesOfType(
            type, [&](BucketEntry const& be) -> Loop {
                if (isStopping())
                {
                    return Loop::COMPLETE;
                }

                if (be.type() == LIVEENTRY || be.type() == INITENTRY)
                {
                    if (!processEntryIfNew(
                            be.liveEntry(), LedgerEntryKey(be.liveEntry()),
                            countedKeys, asset, assetContractInfo, sumBalance,
                            errorMsg))
                    {
                        return Loop::COMPLETE;
                    }
                }
                else if (be.type() == DEADENTRY)
                {
                    countedKeys.emplace(be.deadEntry());
                }
                return Loop::INCOMPLETE;
            });

        if (!errorMsg.empty())
        {
            return;
        }
    }
}

static Loop
scanHotArchiveBucket(HotArchiveBucketSnapshot const& bucket,
                     std::unordered_set<LedgerKey>& countedKeys,
                     Asset const& asset,
                     AssetContractInfo const& assetContractInfo,
                     int64_t& sumBalance, std::string& errorMsg,
                     std::function<bool()> const& isStopping)
{
    for (HotArchiveBucketInputIterator iter(bucket.getRawBucket()); iter;
         ++iter)
    {
        // Allow early termination if application is stopping
        if (isStopping())
        {
            return Loop::COMPLETE;
        }

        auto const& be = *iter;
        if (be.type() == HOT_ARCHIVE_ARCHIVED)
        {
            if (!canHoldAsset(be.archivedEntry().data.type(), asset))
            {
                continue;
            }
            if (!processEntryIfNew(be.archivedEntry(),
                                   LedgerEntryKey(be.archivedEntry()),
                                   countedKeys, asset, assetContractInfo,
                                   sumBalance, errorMsg))
            {
                return Loop::COMPLETE;
            }
        }
        else if (be.type() == HOT_ARCHIVE_LIVE &&
                 canHoldAsset(be.key().type(), asset))
        {
            // HOT_ARCHIVE_LIVE means entry was restored from archive,
            // so mark it as seen (shadowing any archived versions)
            countedKeys.emplace(be.key());
        }
    }
    return Loop::INCOMPLETE;
}

std::string
ConservationOfLumens::checkSnapshot(
    CompleteConstLedgerStatePtr ledgerState,
    InMemorySorobanState const& inMemorySnapshot,
    std::function<bool()> isStopping)
{
    LogSlowExecution logSlow("ConservationOfLumens::checkSnapshot",
                             LogSlowExecution::Mode::AUTOMATIC_RAII, "took",
                             std::chrono::seconds(90));

    auto liveSnapshot = ledgerState->getBucketSnapshot();
    auto hotArchiveSnapshot = ledgerState->getHotArchiveSnapshot();
    auto const& header = liveSnapshot->getLedgerHeader();

    // This invariant can fail prior to v24 due to bugs
    if (protocolVersionIsBefore(header.ledgerVersion, ProtocolVersion::V_24))
    {
        return std::string{};
    }

    Asset nativeAsset(ASSET_TYPE_NATIVE);

    int64_t sumBalance = 0;
    std::string errorMsg;

    // Start with the fee pool from the ledger header
    if (!addBalance(sumBalance, header.feePool))
    {
        return fmt::format(
            FMT_STRING("ConservationOfLumens invariant failed: "
                       "Fee pool balance overflowed when added to total. "
                       "Current sum: {}, Fee pool: {}"),
            sumBalance, header.feePool);
    }

    // Scan the Live BucketList for native balances using loopAllBuckets

    scanLiveBuckets(liveSnapshot, nativeAsset, mLumenContractInfo, sumBalance,
                    errorMsg, isStopping);

    if (!errorMsg.empty())
    {
        return errorMsg;
    }

    // Scan the Hot Archive for native balances using loopAllBuckets
    {
        std::unordered_set<LedgerKey> countedKeys;
        hotArchiveSnapshot->loopAllBuckets(
            [&countedKeys, &nativeAsset, &sumBalance, &errorMsg, &isStopping,
             this](HotArchiveBucketSnapshot const& bucket) {
                return scanHotArchiveBucket(bucket, countedKeys, nativeAsset,
                                            mLumenContractInfo, sumBalance,
                                            errorMsg, isStopping);
            });

        if (!errorMsg.empty())
        {
            return errorMsg;
        }
    }

    // We stopped early, so it's likely we didn't finish scanning everything
    if (isStopping())
    {
        return std::string{};
    }

    // Compare the calculated total with totalCoins from the ledger header
    if (sumBalance != header.totalCoins)
    {
        return fmt::format(
            FMT_STRING("ConservationOfLumens invariant failed: "
                       "Total native asset supply mismatch. "
                       "Calculated from buckets: {}, Expected (totalCoins): "
                       "{}, Difference: {}"),
            sumBalance, header.totalCoins, header.totalCoins - sumBalance);
    }
    return std::string{};
}
}
