// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/ConservationOfLumens.h"
#include "invariant/InvariantManager.h"
#include "ledger/LedgerTxn.h"
#include "main/Application.h"
#include "util/GlobalChecks.h"
#include <fmt/format.h>
#include <numeric>

namespace stellar
{

static int64_t
calculateDeltaBalance(LedgerEntry const* current, LedgerEntry const* previous)
{
    releaseAssert(current || previous);
    auto let = current ? current->data.type() : previous->data.type();
    switch (let)
    {
    case ACCOUNT:
        return (current ? current->data.account().balance : 0) -
               (previous ? previous->data.account().balance : 0);
    case TRUSTLINE:
        break;
    case OFFER:
        break;
    case DATA:
        break;
    case CLAIMABLE_BALANCE:
    {
        auto const& asset = current ? current->data.claimableBalance().asset
                                    : previous->data.claimableBalance().asset;

        if (asset.type() != ASSET_TYPE_NATIVE)
        {
            return 0;
        }

        return ((current ? current->data.claimableBalance().amount : 0) -
                (previous ? previous->data.claimableBalance().amount : 0));
    }
    case LIQUIDITY_POOL:
    {
        auto const* currentBody =
            current ? &current->data.liquidityPool().body.constantProduct()
                    : nullptr;
        auto const* previousBody =
            previous ? &previous->data.liquidityPool().body.constantProduct()
                     : nullptr;

        auto const& assetA =
            (currentBody ? currentBody : previousBody)->params.assetA;
        auto const& assetB =
            (currentBody ? currentBody : previousBody)->params.assetB;

        int64_t delta = 0;
        if (assetA.type() == ASSET_TYPE_NATIVE)
        {
            delta += (currentBody ? currentBody->reserveA : 0) -
                     (previousBody ? previousBody->reserveA : 0);
        }
        if (assetB.type() == ASSET_TYPE_NATIVE)
        {
            delta += (currentBody ? currentBody->reserveB : 0) -
                     (previousBody ? previousBody->reserveB : 0);
        }
        return delta;
    }
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    case CONTRACT_DATA:
        break;
    case CONTRACT_CODE:
        break;
    case CONFIG_SETTING:
        break;
#endif
    }
    return 0;
}

static int64_t
calculateDeltaBalance(
    std::shared_ptr<InternalLedgerEntry const> const& genCurrent,
    std::shared_ptr<InternalLedgerEntry const> const& genPrevious)
{
    auto type = genCurrent ? genCurrent->type() : genPrevious->type();
    if (type == InternalLedgerEntryType::LEDGER_ENTRY)
    {
        auto const* current = genCurrent ? &genCurrent->ledgerEntry() : nullptr;
        auto const* previous =
            genPrevious ? &genPrevious->ledgerEntry() : nullptr;
        return calculateDeltaBalance(current, previous);
    }
    return 0;
}

ConservationOfLumens::ConservationOfLumens() : Invariant(false)
{
}

std::shared_ptr<Invariant>
ConservationOfLumens::registerInvariant(Application& app)
{
    return app.getInvariantManager().registerInvariant<ConservationOfLumens>();
}

std::string
ConservationOfLumens::getName() const
{
    return "ConservationOfLumens";
}

std::string
ConservationOfLumens::checkOnOperationApply(Operation const& operation,
                                            OperationResult const& result,
                                            LedgerTxnDelta const& ltxDelta)
{
    auto const& lhCurr = ltxDelta.header.current;
    auto const& lhPrev = ltxDelta.header.previous;

    int64_t deltaTotalCoins = lhCurr.totalCoins - lhPrev.totalCoins;
    int64_t deltaFeePool = lhCurr.feePool - lhPrev.feePool;
    int64_t deltaBalances = std::accumulate(
        ltxDelta.entry.begin(), ltxDelta.entry.end(), static_cast<int64_t>(0),
        [](int64_t lhs, decltype(ltxDelta.entry)::value_type const& rhs) {
            return lhs + stellar::calculateDeltaBalance(rhs.second.current,
                                                        rhs.second.previous);
        });

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
