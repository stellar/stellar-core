// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/ConservationOfLumens.h"
#include "crypto/SHA.h"
#include "invariant/InvariantManager.h"
#include "ledger/LedgerTxn.h"
#include "main/Application.h"
#include "transactions/TransactionUtils.h"
#include "util/GlobalChecks.h"
#include <fmt/format.h>
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
}
