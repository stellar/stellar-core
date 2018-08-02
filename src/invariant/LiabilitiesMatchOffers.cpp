// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/LiabilitiesMatchOffers.h"
#include "invariant/InvariantManager.h"
#include "ledger/LedgerManager.h"
#include "ledger/OfferFrame.h"
#include "lib/util/format.h"
#include "main/Application.h"
#include "util/types.h"
#include "xdrpp/printer.h"

namespace stellar
{

std::shared_ptr<Invariant>
LiabilitiesMatchOffers::registerInvariant(Application& app)
{
    return app.getInvariantManager().registerInvariant<LiabilitiesMatchOffers>(
        app.getLedgerManager());
}

LiabilitiesMatchOffers::LiabilitiesMatchOffers(LedgerManager& lm)
    : Invariant(false), mLedgerManager(lm)
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
                                              LedgerDelta const& delta)
{
    if (delta.getHeader().ledgerVersion >= 10)
    {
        std::map<AccountID, std::map<Asset, Liabilities>> deltaLiabilities;
        for (auto iter = delta.added().begin(); iter != delta.added().end();
             ++iter)
        {
            auto checkAuthStr = checkAuthorized(iter);
            if (!checkAuthStr.empty())
            {
                return checkAuthStr;
            }
            addCurrentLiabilities(deltaLiabilities, iter);
        }
        for (auto iter = delta.modified().begin();
             iter != delta.modified().end(); ++iter)
        {
            auto checkAuthStr = checkAuthorized(iter);
            if (!checkAuthStr.empty())
            {
                return checkAuthStr;
            }
            addCurrentLiabilities(deltaLiabilities, iter);
            subtractPreviousLiabilities(deltaLiabilities, iter);
        }
        for (auto iter = delta.deleted().begin(); iter != delta.deleted().end();
             ++iter)
        {
            subtractPreviousLiabilities(deltaLiabilities, iter);
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

    auto ledgerVersion = mLedgerManager.getCurrentLedgerVersion();
    for (auto iter = delta.added().begin(); iter != delta.added().end(); ++iter)
    {
        auto msg = checkBalanceAndLimit(iter, ledgerVersion);
        if (!msg.empty())
        {
            return msg;
        }
    }
    for (auto iter = delta.modified().begin(); iter != delta.modified().end();
         ++iter)
    {
        auto msg = checkBalanceAndLimit(iter, ledgerVersion);
        if (!msg.empty())
        {
            return msg;
        }
    }
    return {};
}

template <typename IterType>
void
LiabilitiesMatchOffers::addCurrentLiabilities(
    std::map<AccountID, std::map<Asset, Liabilities>>& deltaLiabilities,
    IterType const& iter) const
{
    auto const& current = iter->current->mEntry;
    if (current.data.type() == ACCOUNT)
    {
        auto const& account = current.data.account();
        Asset native(ASSET_TYPE_NATIVE);
        deltaLiabilities[account.accountID][native].selling -=
            getSellingLiabilities(account, mLedgerManager);
        deltaLiabilities[account.accountID][native].buying -=
            getBuyingLiabilities(account, mLedgerManager);
    }
    else if (current.data.type() == TRUSTLINE)
    {
        auto const& trust = current.data.trustLine();
        deltaLiabilities[trust.accountID][trust.asset].selling -=
            getSellingLiabilities(trust, mLedgerManager);
        deltaLiabilities[trust.accountID][trust.asset].buying -=
            getBuyingLiabilities(trust, mLedgerManager);
    }
    else if (current.data.type() == OFFER)
    {
        auto const& offer = current.data.offer();
        if (offer.selling.type() == ASSET_TYPE_NATIVE ||
            !(getIssuer(offer.selling) == offer.sellerID))
        {
            deltaLiabilities[offer.sellerID][offer.selling].selling +=
                getSellingLiabilities(offer);
        }
        if (offer.buying.type() == ASSET_TYPE_NATIVE ||
            !(getIssuer(offer.buying) == offer.sellerID))
        {
            deltaLiabilities[offer.sellerID][offer.buying].buying +=
                getBuyingLiabilities(offer);
        }
    }
}

template <typename IterType>
void
LiabilitiesMatchOffers::subtractPreviousLiabilities(
    std::map<AccountID, std::map<Asset, Liabilities>>& deltaLiabilities,
    IterType const& iter) const
{
    auto const& previous = iter->previous->mEntry;
    if (previous.data.type() == ACCOUNT)
    {
        auto const& account = previous.data.account();
        Asset native(ASSET_TYPE_NATIVE);
        deltaLiabilities[account.accountID][native].selling +=
            getSellingLiabilities(account, mLedgerManager);
        deltaLiabilities[account.accountID][native].buying +=
            getBuyingLiabilities(account, mLedgerManager);
    }
    else if (previous.data.type() == TRUSTLINE)
    {
        auto const& trust = previous.data.trustLine();
        deltaLiabilities[trust.accountID][trust.asset].selling +=
            getSellingLiabilities(trust, mLedgerManager);
        deltaLiabilities[trust.accountID][trust.asset].buying +=
            getBuyingLiabilities(trust, mLedgerManager);
    }
    else if (previous.data.type() == OFFER)
    {
        auto const& offer = previous.data.offer();
        if (offer.selling.type() == ASSET_TYPE_NATIVE ||
            !(getIssuer(offer.selling) == offer.sellerID))
        {
            deltaLiabilities[offer.sellerID][offer.selling].selling -=
                getSellingLiabilities(offer);
        }
        if (offer.buying.type() == ASSET_TYPE_NATIVE ||
            !(getIssuer(offer.buying) == offer.sellerID))
        {
            deltaLiabilities[offer.sellerID][offer.buying].buying -=
                getBuyingLiabilities(offer);
        }
    }
}

bool
LiabilitiesMatchOffers::shouldCheckAccount(
    LedgerDelta::AddedLedgerEntry const& ale, uint32_t ledgerVersion) const
{
    return true;
}

bool
LiabilitiesMatchOffers::shouldCheckAccount(
    LedgerDelta::ModifiedLedgerEntry const& mle, uint32_t ledgerVersion) const
{
    auto const& current = mle.current->mEntry;
    auto const& previous = mle.previous->mEntry;
    assert(previous.data.type() == ACCOUNT);

    auto const& currAcc = current.data.account();
    auto const& prevAcc = previous.data.account();

    bool didBalanceDecrease = currAcc.balance < prevAcc.balance;
    if (ledgerVersion >= 10)
    {
        bool sellingLiabilitiesInc =
            getSellingLiabilities(currAcc, mLedgerManager) >
            getSellingLiabilities(prevAcc, mLedgerManager);
        bool buyingLiabilitiesInc =
            getBuyingLiabilities(currAcc, mLedgerManager) >
            getBuyingLiabilities(prevAcc, mLedgerManager);
        bool didLiabilitiesIncrease =
            sellingLiabilitiesInc || buyingLiabilitiesInc;
        return didBalanceDecrease || didLiabilitiesIncrease;
    }
    else
    {
        return didBalanceDecrease;
    }
}

template <typename IterType>
std::string
LiabilitiesMatchOffers::checkAuthorized(IterType const& iter) const
{
    auto const& current = iter->current->mEntry;
    if (current.data.type() == TRUSTLINE)
    {
        auto const& trust = current.data.trustLine();
        if (!(trust.flags & AUTHORIZED_FLAG))
        {
            if (getSellingLiabilities(trust, mLedgerManager) > 0 ||
                getBuyingLiabilities(trust, mLedgerManager) > 0)
            {
                return fmt::format("Unauthorized trust line has liabilities {}",
                                   xdr::xdr_to_string(trust));
            }
        }
    }
    return "";
}

template <typename IterType>
std::string
LiabilitiesMatchOffers::checkBalanceAndLimit(IterType const& iter,
                                             uint32_t ledgerVersion) const
{
    auto const& current = iter->current->mEntry;
    if (current.data.type() == ACCOUNT)
    {
        if (shouldCheckAccount(*iter, ledgerVersion))
        {
            auto const& account = current.data.account();
            Liabilities liabilities;
            if (ledgerVersion >= 10)
            {
                liabilities.selling =
                    getSellingLiabilities(account, mLedgerManager);
                liabilities.buying =
                    getBuyingLiabilities(account, mLedgerManager);
            }
            int64_t minBalance =
                mLedgerManager.getMinBalance(account.numSubEntries);
            if ((account.balance < minBalance + liabilities.selling) ||
                (INT64_MAX - account.balance < liabilities.buying))
            {
                return fmt::format(
                    "Balance not compatible with liabilities for account {}",
                    xdr::xdr_to_string(account));
            }
        }
    }
    else if (current.data.type() == TRUSTLINE)
    {
        auto const& trust = current.data.trustLine();
        Liabilities liabilities;
        if (ledgerVersion >= 10)
        {
            liabilities.selling = getSellingLiabilities(trust, mLedgerManager);
            liabilities.buying = getBuyingLiabilities(trust, mLedgerManager);
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
}
