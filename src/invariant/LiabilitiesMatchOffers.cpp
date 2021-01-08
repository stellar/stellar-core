// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/LiabilitiesMatchOffers.h"
#include "invariant/InvariantManager.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "main/Application.h"
#include "transactions/OfferExchange.h"
#include "transactions/TransactionUtils.h"
#include "util/XDRCereal.h"
#include "util/types.h"
#include "xdrpp/printer.h"
#include <fmt/format.h>

namespace stellar
{

static int64_t
getOfferBuyingLiabilities(LedgerEntry const& le)
{
    auto const& oe = le.data.offer();
    auto res = exchangeV10WithoutPriceErrorThresholds(
        oe.price, oe.amount, INT64_MAX, INT64_MAX, INT64_MAX,
        RoundingType::NORMAL);
    return res.numSheepSend;
}

static int64_t
getOfferSellingLiabilities(LedgerEntry const& le)
{
    auto const& oe = le.data.offer();
    auto res = exchangeV10WithoutPriceErrorThresholds(
        oe.price, oe.amount, INT64_MAX, INT64_MAX, INT64_MAX,
        RoundingType::NORMAL);
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
checkAuthorized(LedgerEntry const* current, LedgerEntry const* previous)
{
    if (!current)
    {
        return "";
    }

    if (current->data.type() == TRUSTLINE)
    {
        if (!isAuthorized(*current))
        {
            auto const& trust = current->data.trustLine();
            if (isAuthorizedToMaintainLiabilities(*current))
            {
                auto curSellingLiabilities = getSellingLiabilities(*current);
                auto curBuyingLiabilities = getBuyingLiabilities(*current);

                bool sellingLiabilitiesInc =
                    previous ? curSellingLiabilities >
                                   getSellingLiabilities(*previous)
                             : curSellingLiabilities > 0;
                bool buyingLiabilitiesInc =
                    previous
                        ? curBuyingLiabilities > getBuyingLiabilities(*previous)
                        : curBuyingLiabilities > 0;

                if (sellingLiabilitiesInc || buyingLiabilitiesInc)
                {
                    return fmt::format(
                        "Liabilities increased on unauthorized trust line {}",
                        xdr_to_string(trust, "TrustLineEntry"));
                }
            }
            else
            {
                if (getSellingLiabilities(*current) > 0 ||
                    getBuyingLiabilities(*current) > 0)
                {
                    return fmt::format(
                        "Unauthorized trust line has liabilities {}",
                        xdr_to_string(trust, "TrustLineEntry"));
                }
            }
        }
    }
    return "";
}

static std::string
checkAuthorized(std::shared_ptr<InternalLedgerEntry const> const& genCurrent,
                std::shared_ptr<InternalLedgerEntry const> const& genPrevious)
{
    auto type = genCurrent ? genCurrent->type() : genPrevious->type();
    if (type == InternalLedgerEntryType::LEDGER_ENTRY)
    {
        auto const* current = genCurrent ? &genCurrent->ledgerEntry() : nullptr;
        auto const* previous =
            genPrevious ? &genPrevious->ledgerEntry() : nullptr;
        return checkAuthorized(current, previous);
    }
    return "";
}

static void
addOrSubtractLiabilities(
    std::map<AccountID, std::map<Asset, Liabilities>>& deltaLiabilities,
    LedgerEntry const* entry, bool isAdd)
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
addOrSubtractLiabilities(
    std::map<AccountID, std::map<Asset, Liabilities>>& deltaLiabilities,
    std::shared_ptr<InternalLedgerEntry const> const& genEntry, bool isAdd)
{
    if (genEntry && genEntry->type() == InternalLedgerEntryType::LEDGER_ENTRY)
    {
        addOrSubtractLiabilities(deltaLiabilities, &genEntry->ledgerEntry(),
                                 isAdd);
    }
}

static void
accumulateLiabilities(
    std::map<AccountID, std::map<Asset, Liabilities>>& deltaLiabilities,
    std::shared_ptr<InternalLedgerEntry const> const& current,
    std::shared_ptr<InternalLedgerEntry const> const& previous)
{
    addOrSubtractLiabilities(deltaLiabilities, current, true);
    addOrSubtractLiabilities(deltaLiabilities, previous, false);
}

static bool
shouldCheckAccount(LedgerEntry const* current, LedgerEntry const* previous,
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
            getSellingLiabilities(*current) > getSellingLiabilities(*previous);
        bool buyingLiabilitiesInc =
            getBuyingLiabilities(*current) > getBuyingLiabilities(*previous);
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
checkBalanceAndLimit(LedgerHeader const& header, LedgerEntry const* current,
                     LedgerEntry const* previous, uint32_t ledgerVersion)
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
            int64_t minBalance = getMinBalance(header, account);
            if ((account.balance < minBalance + liabilities.selling) ||
                (INT64_MAX - account.balance < liabilities.buying))
            {
                return fmt::format(
                    "Balance not compatible with liabilities for {}",
                    xdr_to_string(account, "AccountEntry"));
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
            return fmt::format("Balance not compatible with liabilities for {}",
                               xdr_to_string(trust, "TrustLineEntry"));
        }
    }
    return {};
}

static std::string
checkBalanceAndLimit(
    LedgerHeader const& header,
    std::shared_ptr<InternalLedgerEntry const> const& genCurrent,
    std::shared_ptr<InternalLedgerEntry const> const& genPrevious,
    uint32_t ledgerVersion)
{
    auto type = genCurrent ? genCurrent->type() : genPrevious->type();
    if (type == InternalLedgerEntryType::LEDGER_ENTRY)
    {
        auto const* current = genCurrent ? &genCurrent->ledgerEntry() : nullptr;
        auto const* previous =
            genPrevious ? &genPrevious->ledgerEntry() : nullptr;
        return checkBalanceAndLimit(header, current, previous, ledgerVersion);
    }
    return "";
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
    return "LiabilitiesMatchOffers";
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
            auto checkAuthStr = checkAuthorized(entryDelta.second.current,
                                                entryDelta.second.previous);
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
                        "offers by {} for {} in {}",
                        assetLiabilities.second.buying,
                        xdr_to_string(accLiabilities.first, "account"),
                        xdr_to_string(assetLiabilities.first, "asset"));
                }
                else if (assetLiabilities.second.selling != 0)
                {
                    return fmt::format(
                        "Change in selling liabilities differed from "
                        "change in total selling liabilities of "
                        "offers by {} for {} in {}",
                        assetLiabilities.second.selling,
                        xdr_to_string(accLiabilities.first, "account"),
                        xdr_to_string(assetLiabilities.first, "asset"));
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
