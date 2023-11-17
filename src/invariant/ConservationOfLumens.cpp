// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/ConservationOfLumens.h"
#include "crypto/SHA.h"
#include "invariant/InvariantManager.h"
#include "ledger/LedgerTxn.h"
#include "main/Application.h"
#include "util/GlobalChecks.h"
#include <fmt/format.h>
#include <numeric>

namespace stellar
{

static int64_t
calculateDeltaBalance(LumenContractInfo const& lumenContractInfo,
                      LedgerEntry const* current, LedgerEntry const* previous)
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
    case CONTRACT_DATA:
    {
        auto const& contractData = current ? current->data.contractData()
                                           : previous->data.contractData();
        if (contractData.contract.type() != SC_ADDRESS_TYPE_CONTRACT ||
            contractData.contract.contractId() !=
                lumenContractInfo.mLumenContractID ||
            contractData.key.type() != SCV_VEC || !contractData.key.vec() ||
            contractData.key.vec().size() == 0)
        {
            return 0;
        }

        // The balanceSymbol should be the first entry in the SCVec
        if (!(contractData.key.vec()->at(0) ==
              lumenContractInfo.mBalanceSymbol))
        {
            return 0;
        }

        auto getAmount =
            [&lumenContractInfo](LedgerEntry const* entry) -> int64_t {
            if (!entry)
            {
                return 0;
            }

            // The amount should be the first entry in the SCMap
            auto const& val = entry->data.contractData().val;
            if (val.type() == SCV_MAP && val.map() && val.map()->size() != 0)
            {
                auto const& amountEntry = val.map()->at(0);
                if (amountEntry.key == lumenContractInfo.mAmountSymbol)
                {
                    if (amountEntry.val.type() == SCV_I128)
                    {
                        auto lo = amountEntry.val.i128().lo;
                        auto hi = amountEntry.val.i128().hi;
                        if (lo > INT64_MAX || hi > 0)
                        {
                            // The amount isn't right, but it'll trigger the
                            // invariant.
                            return INT64_MAX;
                        }
                        return static_cast<int64_t>(lo);
                    }
                }
            }
            return 0;
        };

        return getAmount(current) - getAmount(previous);
    }
    case CONTRACT_CODE:
        break;
    case CONFIG_SETTING:
        break;
    case TTL:
        break;
    }
    return 0;
}

static int64_t
calculateDeltaBalance(
    LumenContractInfo const& lumenContractInfo,
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
    LumenContractInfo const& lumenContractInfo)
    : Invariant(false), mLumenContractInfo(lumenContractInfo)
{
}

std::shared_ptr<Invariant>
ConservationOfLumens::registerInvariant(Application& app)
{
    // We need to keep track of lumens in the Stellar Asset Contract, so
    // calculate the lumen contractID, the key of the Balance entry, and the
    // amount field within that entry.
    auto lumenInfo = getLumenContractInfo(app.getNetworkID());

    return app.getInvariantManager().registerInvariant<ConservationOfLumens>(
        lumenInfo);
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
        [this](int64_t lhs, decltype(ltxDelta.entry)::value_type const& rhs) {
            return lhs + stellar::calculateDeltaBalance(mLumenContractInfo,
                                                        rhs.second.current,
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
