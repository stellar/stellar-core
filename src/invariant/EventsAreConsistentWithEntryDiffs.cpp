// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/EventsAreConsistentWithEntryDiffs.h"
#include "crypto/SHA.h"
#include "invariant/InvariantManager.h"
#include "ledger/LedgerTxn.h"
#include "main/Application.h"
#include "rust/RustBridge.h"
#include "transactions/EventManager.h"
#include "util/GlobalChecks.h"
#include <fmt/format.h>
#include <numeric>

namespace stellar
{

namespace
{
static const CxxI128 I128ZERO{0, 0};

struct AggregatedEvents
{
    UnorderedMap<SCAddress, UnorderedMap<Asset, CxxI128>> mEventAmounts;
    UnorderedMap<SCAddress, UnorderedMap<Asset, std::optional<bool>>>
        mIsAuthorized;

    bool addAssetBalance(SCAddress const& addr, Asset const& asset,
                         CxxI128 const& amount);

    bool subtractAssetBalance(SCAddress const& addr, Asset const& asset,
                              CxxI128 const& amount);

    CxxI128 consumeAmount(SCAddress const& addr, Asset const& asset);
};

bool
AggregatedEvents::addAssetBalance(SCAddress const& addr, Asset const& asset,
                                  CxxI128 const& amount)
{
    auto current = mEventAmounts[addr][asset];

    if (rust_bridge::i128_add_will_overflow(current, amount))
    {
        return false;
    }

    mEventAmounts[addr][asset] = rust_bridge::i128_add(current, amount);
    return true;
}

bool
AggregatedEvents::subtractAssetBalance(SCAddress const& addr,
                                       Asset const& asset,
                                       CxxI128 const& amount)
{
    auto current = mEventAmounts[addr][asset];

    if (rust_bridge::i128_sub_will_underflow(current, amount))
    {
        return false;
    }

    mEventAmounts[addr][asset] = rust_bridge::i128_sub(current, amount);
    return true;
}

CxxI128
AggregatedEvents::consumeAmount(SCAddress const& addr, Asset const& asset)
{
    auto lkAssetMapIt = mEventAmounts.find(addr);
    if (lkAssetMapIt == mEventAmounts.end())
    {
        return I128ZERO;
    }

    auto& lkAssetMap = lkAssetMapIt->second;
    auto assetAmountIt = lkAssetMap.find(asset);
    if (assetAmountIt == lkAssetMap.end())
    {
        return I128ZERO;
    }

    auto res = assetAmountIt->second;

    // Now remove this value from the map
    lkAssetMap.erase(assetAmountIt);
    if (lkAssetMap.empty())
    {
        mEventAmounts.erase(lkAssetMapIt);
    }
    return res;
}

CxxI128
getAmountFromData(SCVal const& data)
{
    if (data.type() == SCV_I128)
    {
        auto const& amountVal = data.i128();
        return CxxI128{amountVal.hi, amountVal.lo};
    }
    else if (data.type() == SCV_MAP && data.map())
    {
        auto const& map = *data.map();
        for (auto const& entry : map)
        {
            if (entry.key.type() == SCV_SYMBOL && entry.key.sym() == "amount" &&
                entry.val.type() == SCV_I128)
            {
                auto const& amountVal = entry.val.i128();
                return CxxI128{amountVal.hi, amountVal.lo};
            }
        }
    }
    return I128ZERO;
}

std::optional<SCAddress>
getAddressFromBalanceKey(LedgerKey const& lk)
{
    if (lk.contractData().key.type() != SCV_VEC)
    {
        return std::nullopt;
    }

    auto const& vec = lk.contractData().key.vec();
    if (!vec || vec->size() != 2)
    {
        return std::nullopt;
    }

    auto const& addr = vec->at(1);

    if (addr.type() != SCV_ADDRESS)
    {
        return std::nullopt;
    }

    return addr.address();
}

// Should only be called with trustlines as input
bool
checkAuthorization(AggregatedEvents& agg, SCAddress const& trustlineOwner,
                   Asset const& asset, LedgerEntry const* current,
                   LedgerEntry const* previous)
{
    if (!current || !previous)
    {
        // If a trustline was created or deleted auth could not have changed.
        return true;
    }

    bool currAuth = isAuthorized(*current);
    bool prevAuth = isAuthorized(*previous);

    std::optional<bool> eventAuth;

    auto assetMapIt = agg.mIsAuthorized.find(trustlineOwner);
    if (assetMapIt != agg.mIsAuthorized.end())
    {
        auto authIt = assetMapIt->second.find(asset);
        if (authIt != assetMapIt->second.end())
        {
            eventAuth = authIt->second;
        }
    }

    auto res = true;

    // if event was seen, make sure it matches current state
    if (eventAuth)
    {
        res = (*eventAuth == currAuth);
    }

    // if auth on trustline changed, make sure an event was emitted
    if (res && currAuth != prevAuth)
    {
        res = eventAuth.has_value();
    }
    return res;
}

std::string
calculateDeltaBalance(AggregatedEvents& agg,
                      UnorderedMap<Hash, Asset> const& stellarAssetContractIDs,
                      LedgerEntry const* current, LedgerEntry const* previous)
{
    releaseAssert(current || previous);
    auto lk = current ? LedgerEntryKey(*current) : LedgerEntryKey(*previous);
    auto let = current ? current->data.type() : previous->data.type();

    switch (let)
    {
    case ACCOUNT:
    {
        Asset native(ASSET_TYPE_NATIVE);

        auto eventDiff = agg.consumeAmount(
            makeAccountAddress(lk.account().accountID), native);

        auto entryDiff = (current ? current->data.account().balance : 0) -
                         (previous ? previous->data.account().balance : 0);

        return rust_bridge::i128_from_i64(entryDiff) == eventDiff
                   ? ""
                   : "Account diff does not match events";
    }
    case TRUSTLINE:
    {
        auto const& tlAsset = current ? current->data.trustLine().asset
                                      : previous->data.trustLine().asset;

        Asset asset;
        switch (tlAsset.type())
        {
        case stellar::ASSET_TYPE_CREDIT_ALPHANUM4:
            asset.type(ASSET_TYPE_CREDIT_ALPHANUM4);
            asset.alphaNum4() = tlAsset.alphaNum4();
            break;
        case stellar::ASSET_TYPE_CREDIT_ALPHANUM12:
            asset.type(ASSET_TYPE_CREDIT_ALPHANUM12);
            asset.alphaNum12() = tlAsset.alphaNum12();
            break;
        case stellar::ASSET_TYPE_POOL_SHARE:
            return "";
        case stellar::ASSET_TYPE_NATIVE:
        default:
            return "Invalid asset in trustline";
        }

        auto const& trustlineOwner = current
                                         ? current->data.trustLine().accountID
                                         : previous->data.trustLine().accountID;

        auto tlOwnerAddress = makeAccountAddress(trustlineOwner);
        if (!checkAuthorization(agg, tlOwnerAddress, asset, current, previous))
        {
            return "trustline authorization and events do not match";
        }

        auto eventDiff = agg.consumeAmount(tlOwnerAddress, asset);

        auto entryDiff = (current ? current->data.trustLine().balance : 0) -
                         (previous ? previous->data.trustLine().balance : 0);

        return rust_bridge::i128_from_i64(entryDiff) == eventDiff
                   ? ""
                   : "Trustline diff does not match events";
    }
    case OFFER:
        break;
    case DATA:
        break;
    case CLAIMABLE_BALANCE:
    {
        auto const& asset = current ? current->data.claimableBalance().asset
                                    : previous->data.claimableBalance().asset;

        auto eventDiff = agg.consumeAmount(
            makeClaimableBalanceAddress(lk.claimableBalance().balanceID),
            asset);
        auto entryDiff =
            (current ? current->data.claimableBalance().amount : 0) -
            (previous ? previous->data.claimableBalance().amount : 0);

        return rust_bridge::i128_from_i64(entryDiff) == eventDiff
                   ? ""
                   : "ClaimableBalance diff does not match events";
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

        int64_t entryADiff = (currentBody ? currentBody->reserveA : 0) -
                             (previousBody ? previousBody->reserveA : 0);

        int64_t entryBDiff = (currentBody ? currentBody->reserveB : 0) -
                             (previousBody ? previousBody->reserveB : 0);

        auto poolAddress =
            makeLiquidityPoolAddress(lk.liquidityPool().liquidityPoolID);
        auto eventADiff = agg.consumeAmount(poolAddress, assetA);
        auto eventBDiff = agg.consumeAmount(poolAddress, assetB);

        return rust_bridge::i128_from_i64(entryADiff) == eventADiff &&
                       rust_bridge::i128_from_i64(entryBDiff) == eventBDiff
                   ? ""
                   : "LiquidityPool diff does not match events";
    }
    case CONTRACT_DATA:
    {
        auto const& contractData = current ? current->data.contractData()
                                           : previous->data.contractData();

        if (contractData.contract.type() != SC_ADDRESS_TYPE_CONTRACT)
        {
            return "";
        }

        auto assetIt =
            stellarAssetContractIDs.find(contractData.contract.contractId());
        if (assetIt == stellarAssetContractIDs.end())
        {
            return "";
        }

        auto const& asset = assetIt->second;

        // If the key is ContractData, then it is a balance entry. We need to
        // convert it to the address that the balance belongs to.
        auto maybeAddress = getAddressFromBalanceKey(lk);
        auto eventDiff =
            maybeAddress ? agg.consumeAmount(*maybeAddress, asset) : I128ZERO;

        auto getAmount = [](LedgerEntry const* entry) -> CxxI128 {
            if (!entry)
            {
                return I128ZERO;
            }

            // Make sure this is the balance entry and not an allowance
            auto const& dataKey = entry->data.contractData().key;
            if (dataKey.type() != SCV_VEC || !dataKey.vec() ||
                dataKey.vec()->size() != 2)
            {
                return I128ZERO;
            }
            auto const& name = dataKey.vec()->at(0);
            if (name.type() != SCV_SYMBOL || name.sym() != "Balance")
            {
                return I128ZERO;
            }

            // The amount should be the first entry in the SCMap
            auto const& val = entry->data.contractData().val;
            if (val.type() == SCV_MAP && val.map() && val.map()->size() != 0)
            {
                auto const& amountEntry = val.map()->at(0);
                return getAmountFromData(amountEntry.val);
            }
            return I128ZERO;
        };
        auto entryDiff =
            rust_bridge::i128_sub(getAmount(current), getAmount(previous));
        return entryDiff == eventDiff
                   ? ""
                   : "ContractData diff does not match events";
    }
    case CONTRACT_CODE:
        break;
    case CONFIG_SETTING:
        break;
    case TTL:
        break;
    }
    return "";
}

std::string
verifyEventsDelta(AggregatedEvents& agg,
                  UnorderedMap<Hash, Asset> const& stellarAssetContractIDs,
                  std::shared_ptr<InternalLedgerEntry const> const& genCurrent,
                  std::shared_ptr<InternalLedgerEntry const> const& genPrevious)
{
    auto type = genCurrent ? genCurrent->type() : genPrevious->type();
    if (type == InternalLedgerEntryType::LEDGER_ENTRY)
    {
        auto const* current = genCurrent ? &genCurrent->ledgerEntry() : nullptr;
        auto const* previous =
            genPrevious ? &genPrevious->ledgerEntry() : nullptr;

        return calculateDeltaBalance(agg, stellarAssetContractIDs, current,
                                     previous);
    }
    return "";
}

std::optional<AggregatedEvents>
aggregateEventDiffs(Hash const& networkID,
                    UnorderedMap<Hash, Asset>& stellarAssetContractIDs,
                    std::vector<ContractEvent> const& events)
{
    AggregatedEvents res;
    for (auto const& event : events)
    {
        if (!event.contractID)
        {
            continue;
        }
        auto const& topics = event.body.v0().topics;
        Asset asset;
        auto assetIt = stellarAssetContractIDs.find(*event.contractID);
        if (assetIt != stellarAssetContractIDs.end())
        {
            asset = assetIt->second;
        }
        else
        {
            auto maybeAsset = getAssetFromEvent(event, networkID);
            if (maybeAsset)
            {
                asset = *maybeAsset;
                stellarAssetContractIDs.emplace(*event.contractID, asset);
            }
            else
            {
                continue;
            }
        }
        // at this point, we have verified that this is an SAC event

        auto const& eventNameVal = topics.at(0);
        if (eventNameVal.type() != SCV_SYMBOL)
        {
            return std::nullopt;
        }
        if (eventNameVal.sym() == "transfer")
        {
            if (topics.size() != 4)
            {
                return std::nullopt;
            }

            auto const& fromVal = topics.at(1);
            auto const& toVal = topics.at(2);

            auto amount = getAmountFromData(event.body.v0().data);

            // If the events are sane, we should never overflow.
            if (!res.subtractAssetBalance(fromVal.address(), asset, amount))
            {
                return std::nullopt;
            }

            if (!res.addAssetBalance(toVal.address(), asset, amount))
            {
                return std::nullopt;
            }
        }
        else if (eventNameVal.sym() == "mint")
        {
            if (topics.size() != 3)
            {
                return std::nullopt;
            }

            auto const& toVal = topics.at(1);

            auto amount = getAmountFromData(event.body.v0().data);
            if (!res.addAssetBalance(toVal.address(), asset, amount))
            {
                return std::nullopt;
            }
        }
        else if (eventNameVal.sym() == "burn" ||
                 eventNameVal.sym() == "clawback")
        {
            if (topics.size() != 3)
            {
                return std::nullopt;
            }

            auto const& fromVal = topics.at(1);

            auto amount = getAmountFromData(event.body.v0().data);
            if (!res.subtractAssetBalance(fromVal.address(), asset, amount))
            {
                return std::nullopt;
            }
        }
        else if (eventNameVal.sym() == "set_authorized")
        {
            if (topics.size() != 3)
            {
                return std::nullopt;
            }

            auto const& idVal = topics.at(1);

            if (event.body.v0().data.type() != SCV_BOOL)
            {
                return std::nullopt;
            }

            // We only need the value from the last set_authorized event to
            // compare ledger state against, so it is fine to overwrite
            // previously saved values.
            res.mIsAuthorized[idVal.address()][asset] =
                event.body.v0().data.b();
        }
    }
    return res;
}
}

EventsAreConsistentWithEntryDiffs::EventsAreConsistentWithEntryDiffs(
    Hash const& networkID)
    : Invariant(true /*isStrict*/), mNetworkID(networkID)
{
}

std::shared_ptr<Invariant>
EventsAreConsistentWithEntryDiffs::registerInvariant(Application& app)
{
    return app.getInvariantManager()
        .registerInvariant<EventsAreConsistentWithEntryDiffs>(
            app.getNetworkID());
}

std::string
EventsAreConsistentWithEntryDiffs::getName() const
{
    return "EventsAreConsistentWithEntryDiffs";
}

// Note that this invariant only verifies balance changes in the context of an
// operation. The fee events should not be accounted for here.
std::string
EventsAreConsistentWithEntryDiffs::checkOnOperationApply(
    Operation const& operation, OperationResult const& result,
    LedgerTxnDelta const& ltxDelta, std::vector<ContractEvent> const& events)
{
    UnorderedMap<Hash, Asset> stellarAssetContractIDs;
    auto maybeAggregatedEventAmounts =
        aggregateEventDiffs(mNetworkID, stellarAssetContractIDs, events);
    if (!maybeAggregatedEventAmounts)
    {
        return "received invalid events";
    }

    for (auto const& delta : ltxDelta.entry)
    {
        auto res = verifyEventsDelta(
            *maybeAggregatedEventAmounts, stellarAssetContractIDs,
            delta.second.current, delta.second.previous);
        if (!res.empty())
        {
            return res;
        }
    }

    for (auto const& kvp : maybeAggregatedEventAmounts->mEventAmounts)
    {
        for (auto const& kvp2 : kvp.second)
        {
            if (kvp2.second != I128ZERO)
            {
                return "Some event diffs not consumed";
            }
        }
    }
    return {};
}
}