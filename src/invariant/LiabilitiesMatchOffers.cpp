// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/LiabilitiesMatchOffers.h"
#include "invariant/InvariantManager.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "lib/util/format.h"
#include "main/Application.h"
#include "transactions/OfferExchange.h"
#include "util/types.h"
#include "xdrpp/printer.h"

namespace stellar
{

static int64_t
getMinBalance(LedgerHeader const& header, uint32_t ownerCount)
{
    if (header.ledgerVersion <= 8)
        return (2 + ownerCount) * header.baseReserve;
    else
        return (2 + ownerCount) * int64_t(header.baseReserve);
}

static int64_t
getOfferBuyingLiabilities(LedgerEntry const& le)
{
    auto const& oe = le.data.offer();
    auto res = exchangeV10WithoutPriceErrorThresholds(
        oe.price, oe.amount, INT64_MAX, INT64_MAX, INT64_MAX, false);
    return res.numSheepSend;
}

static int64_t
getOfferSellingLiabilities(LedgerEntry const& le)
{
    auto const& oe = le.data.offer();
    auto res = exchangeV10WithoutPriceErrorThresholds(
        oe.price, oe.amount, INT64_MAX, INT64_MAX, INT64_MAX, false);
    return res.numWheatReceived;
}

static int64_t
getBuyingLiabilities(LedgerEntry const& le)
{
    if (le.data.type() == ACCOUNT)
    {
        auto const& acc = le.data.account();
        return (acc.ext.v() == 0) ? 0 : acc.ext.v1().liabilities.buying;
    }
    else if (le.data.type() == TRUSTLINE)
    {
        auto const& tl = le.data.trustLine();
        return (tl.ext.v() == 0) ? 0 : tl.ext.v1().liabilities.buying;
    }
    throw std::runtime_error("Unknown LedgerEntry type");
}

int64_t
getSellingLiabilities(LedgerEntry const& le)
{
    if (le.data.type() == ACCOUNT)
    {
        auto const& acc = le.data.account();
        return (acc.ext.v() == 0) ? 0 : acc.ext.v1().liabilities.selling;
    }
    else if (le.data.type() == TRUSTLINE)
    {
        auto const& tl = le.data.trustLine();
        return (tl.ext.v() == 0) ? 0 : tl.ext.v1().liabilities.selling;
    }
    throw std::runtime_error("Unknown LedgerEntry type");
}

static std::string
checkAuthorized(std::shared_ptr<LedgerEntry const> const& current)
{
    if (!current)
    {
        return "";
    }

    if (current->data.type() == TRUSTLINE)
    {
        auto const& trust = current->data.trustLine();
        if (!(trust.flags & AUTHORIZED_FLAG))
        {
            if (getSellingLiabilities(*current) > 0 ||
                getBuyingLiabilities(*current) > 0)
            {
                return fmt::format("Unauthorized trust line has liabilities {}",
                                   xdr::xdr_to_string(trust));
            }
        }
    }
    return "";
}

static void
addOrSubtractLiabilities(
    std::map<AccountID, std::map<Asset, Liabilities>>& deltaLiabilities,
    std::shared_ptr<LedgerEntry const> const& entry, bool isAdd)
{
    if (!entry)
    {
        return;
    }

    int64_t sign = isAdd ? 1 : -1;

    if (entry->data.type() == ACCOUNT)
    {
        auto const& account = entry->data.account();
        Asset native(ASSET_TYPE_NATIVE);
        deltaLiabilities[account.accountID][native].selling -=
            sign * getSellingLiabilities(*entry);
        deltaLiabilities[account.accountID][native].buying -=
            sign * getBuyingLiabilities(*entry);
    }
    else if (entry->data.type() == TRUSTLINE)
    {
        auto const& trust = entry->data.trustLine();
        deltaLiabilities[trust.accountID][trust.asset].selling -=
            sign * getSellingLiabilities(*entry);
        deltaLiabilities[trust.accountID][trust.asset].buying -=
            sign * getBuyingLiabilities(*entry);
    }
    else if (entry->data.type() == OFFER)
    {
        auto const& offer = entry->data.offer();
        if (offer.selling.type() == ASSET_TYPE_NATIVE ||
            !(getIssuer(offer.selling) == offer.sellerID))
        {
            deltaLiabilities[offer.sellerID][offer.selling].selling +=
                sign * getOfferSellingLiabilities(*entry);
        }
        if (offer.buying.type() == ASSET_TYPE_NATIVE ||
            !(getIssuer(offer.buying) == offer.sellerID))
        {
            deltaLiabilities[offer.sellerID][offer.buying].buying +=
                sign * getOfferBuyingLiabilities(*entry);
        }
    }
}

static void
accumulateLiabilities(
    std::map<AccountID, std::map<Asset, Liabilities>>& deltaLiabilities,
    std::shared_ptr<LedgerEntry const> const& current,
    std::shared_ptr<LedgerEntry const> const& previous)
{
    addOrSubtractLiabilities(deltaLiabilities, current, true);
    addOrSubtractLiabilities(deltaLiabilities, previous, false);
}

static bool
shouldCheckAccount(std::shared_ptr<LedgerEntry const> const& current,
                   std::shared_ptr<LedgerEntry const> const& previous,
                   uint32_t ledgerVersion)
{
    if (!previous)
    {
        return true;
    }

    auto const& currAcc = current->data.account();
    auto const& prevAcc = previous->data.account();

    bool didBalanceDecrease = currAcc.balance < prevAcc.balance;
    if (ledgerVersion >= 10)
    {
        bool sellingLiabilitiesInc =
            getSellingLiabilities(*current) > getSellingLiabilities(*current);
        bool buyingLiabilitiesInc =
            getBuyingLiabilities(*current) > getBuyingLiabilities(*current);
        bool didLiabilitiesIncrease =
            sellingLiabilitiesInc || buyingLiabilitiesInc;
        return didBalanceDecrease || didLiabilitiesIncrease;
    }
    else
    {
        return didBalanceDecrease;
    }
}

static std::string
checkBalanceAndLimit(LedgerHeader const& header,
                     std::shared_ptr<LedgerEntry const> const& current,
                     std::shared_ptr<LedgerEntry const> const& previous,
                     uint32_t ledgerVersion)
{
    if (!current)
    {
        return {};
    }

    if (current->data.type() == ACCOUNT)
    {
        if (shouldCheckAccount(current, previous, ledgerVersion))
        {
            auto const& account = current->data.account();
            Liabilities liabilities;
            if (ledgerVersion >= 10)
            {
                liabilities.selling = getSellingLiabilities(*current);
                liabilities.buying = getBuyingLiabilities(*current);
            }
            int64_t minBalance = getMinBalance(header, account.numSubEntries);
            if ((account.balance < minBalance + liabilities.selling) ||
                (INT64_MAX - account.balance < liabilities.buying))
            {
                return fmt::format(
                    "Balance not compatible with liabilities for account {}",
                    xdr::xdr_to_string(account));
            }
        }
    }
    else if (current->data.type() == TRUSTLINE)
    {
        auto const& trust = current->data.trustLine();
        Liabilities liabilities;
        if (ledgerVersion >= 10)
        {
            liabilities.selling = getSellingLiabilities(*current);
            liabilities.buying = getBuyingLiabilities(*current);
        }
        if ((trust.balance < liabilities.selling) ||
            (trust.limit - trust.balance < liabilities.buying))
        {
            return fmt::format(
                "Balance not compatible with liabilities for trustline {}",
                xdr::xdr_to_string(trust));
        }
    }
    return {};
}

std::shared_ptr<Invariant>
LiabilitiesMatchOffers::registerInvariant(Application& app)
{
    return app.getInvariantManager()
        .registerInvariant<LiabilitiesMatchOffers>();
}

LiabilitiesMatchOffers::LiabilitiesMatchOffers() : Invariant(false)
{
}

std::string
LiabilitiesMatchOffers::getName() const
{
    // NOTE: In order for the acceptance tests to run correctly, this will
    // currently need to read "MinimumAccountBalance". We will update this to
    // "LiabilitiesMatchOffers" after.
    return "MinimumAccountBalance";
}

std::string
LiabilitiesMatchOffers::checkOnOperationApply(Operation const& operation,
                                              OperationResult const& result,
                                              LedgerTxnDelta const& ltxDelta)
{
    auto ledgerVersion = ltxDelta.header.current.ledgerVersion;
    if (ledgerVersion >= 10)
    {
        std::map<AccountID, std::map<Asset, Liabilities>> deltaLiabilities;
        for (auto const& entryDelta : ltxDelta.entry)
        {
            auto checkAuthStr =
                stellar::checkAuthorized(entryDelta.second.current);
            if (!checkAuthStr.empty())
            {
                return checkAuthStr;
            }
            accumulateLiabilities(deltaLiabilities, entryDelta.second.current,
                                  entryDelta.second.previous);
        }

        for (auto const& accLiabilities : deltaLiabilities)
        {
            for (auto const& assetLiabilities : accLiabilities.second)
            {
                if (assetLiabilities.second.buying != 0)
                {
                    return fmt::format(
                        "Change in buying liabilities differed from "
                        "change in total buying liabilities of "
                        "offers by {} for account {} in asset {}",
                        assetLiabilities.second.buying,
                        xdr::xdr_to_string(accLiabilities.first),
                        xdr::xdr_to_string(assetLiabilities.first));
                }
                else if (assetLiabilities.second.selling != 0)
                {
                    return fmt::format(
                        "Change in selling liabilities differed from "
                        "change in total selling liabilities of "
                        "offers by {} for account {} in asset {}",
                        assetLiabilities.second.selling,
                        xdr::xdr_to_string(accLiabilities.first),
                        xdr::xdr_to_string(assetLiabilities.first));
                }
            }
        }
    }

    for (auto const& entryDelta : ltxDelta.entry)
    {
        auto msg = stellar::checkBalanceAndLimit(
            ltxDelta.header.current, entryDelta.second.current,
            entryDelta.second.previous, ledgerVersion);
        if (!msg.empty())
        {
            return msg;
        }
    }
    return {};
}
}
