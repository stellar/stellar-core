// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "OfferExchange.h"
#include "database/Database.h"
#include "database/OfferQueries.h"
#include "ledger/AccountFrame.h"
#include "ledgerdelta/LedgerDelta.h"
#include "ledger/LedgerManager.h"
#include "ledger/OfferFrame.h"
#include "ledger/TrustFrame.h"
#include "main/Application.h"
#include "util/Logging.h"

namespace stellar
{

namespace
{

int64_t
canBuyAtMost(Asset const& asset, optional<LedgerEntry const> trustLine, Price const& price)
{
    if (asset.type() == ASSET_TYPE_NATIVE)
    {
        return INT64_MAX;
    }

    // compute value based on what the account can receive
    auto sellerMaxSheep = trustLine ? TrustFrame{*trustLine}.getMaxAmountReceive() : 0;

    auto result = int64_t{};
    if (!bigDivide(result, sellerMaxSheep, price.d, price.n, ROUND_DOWN))
    {
        result = INT64_MAX;
    }

    return result;
}

int64_t
canSellAtMost(AccountFrame &account, Asset const& asset,
              optional<LedgerEntry const> trustLine, LedgerManager& ledgerManager)
{
    if (asset.type() == ASSET_TYPE_NATIVE)
    {
        // can only send above the minimum balance
        return account.getBalanceAboveReserve(ledgerManager);
    }

    if (trustLine && TrustFrame{*trustLine}.isAuthorized())
    {
        return TrustFrame{*trustLine}.getBalance();
    }

    return 0;
}
}

ExchangeResult
exchangeV2(int64_t wheatReceived, Price price, int64_t maxWheatReceive,
           int64_t maxSheepSend)
{
    auto result = ExchangeResult{};
    result.reduced = wheatReceived > maxWheatReceive;
    wheatReceived = std::min(wheatReceived, maxWheatReceive);

    // this guy can get X wheat to you. How many sheep does that get him?
    if (!bigDivide(result.numSheepSend, wheatReceived, price.n, price.d,
                   ROUND_DOWN))
    {
        result.numSheepSend = INT64_MAX;
    }

    result.reduced = result.reduced || (result.numSheepSend > maxSheepSend);
    result.numSheepSend = std::min(result.numSheepSend, maxSheepSend);
    // bias towards seller (this cannot overflow at this point)
    result.numWheatReceived =
        bigDivide(result.numSheepSend, price.d, price.n, ROUND_DOWN);

    return result;
}

ExchangeResult
exchangeV3(int64_t wheatReceived, Price price, int64_t maxWheatReceive,
           int64_t maxSheepSend)
{
    auto result = ExchangeResult{};
    result.reduced = wheatReceived > maxWheatReceive;
    result.numWheatReceived = std::min(wheatReceived, maxWheatReceive);

    // this guy can get X wheat to you. How many sheep does that get him?
    // bias towards seller
    if (!bigDivide(result.numSheepSend, result.numWheatReceived, price.n,
                   price.d, ROUND_UP))
    {
        result.reduced = true;
        result.numSheepSend = INT64_MAX;
    }

    result.reduced = result.reduced || (result.numSheepSend > maxSheepSend);
    result.numSheepSend = std::min(result.numSheepSend, maxSheepSend);

    auto newWheatReceived = int64_t{};
    if (!bigDivide(newWheatReceived, result.numSheepSend, price.d, price.n,
                   ROUND_DOWN))
    {
        newWheatReceived = INT64_MAX;
    }
    result.numWheatReceived =
        std::min(result.numWheatReceived, newWheatReceived);

    return result;
}

OfferExchange::OfferExchange(Application& app)
    : mApp(app)
{
}

OfferExchange::CrossOfferResult
OfferExchange::crossOffer(LedgerDelta& ledgerDelta, OfferFrame sellingWheatOffer,
                          int64_t maxWheatReceived, int64_t& numWheatReceived,
                          int64_t maxSheepSend, int64_t& numSheepSend)
{
    assert(maxWheatReceived > 0);
    assert(maxSheepSend > 0);

    // we're about to make changes to the offer
    ledgerDelta.recordEntry(sellingWheatOffer);

    auto const& sheep = sellingWheatOffer.getBuying();
    auto const& wheat = sellingWheatOffer.getSelling();
    auto const& accountBID = sellingWheatOffer.getSellerID();

    auto accountB = ledgerDelta.loadAccount(accountBID);
    if (!accountB)
    {
        throw std::runtime_error(
            "invalid database state: offer must have matching account");
    }
    auto acocuntBFrame = AccountFrame{*accountB};

    optional<LedgerEntry const> wheatLineAccountB;
    if (wheat.type() != ASSET_TYPE_NATIVE)
    {
        wheatLineAccountB = ledgerDelta.loadTrustLine(accountBID, wheat);
    }

    optional<LedgerEntry const> sheepLineAccountB;
    if (sheep.type() != ASSET_TYPE_NATIVE)
    {
        sheepLineAccountB = ledgerDelta.loadTrustLine(accountBID, sheep);
    }

    auto& ledgerManager = mApp.getLedgerManager();
    numWheatReceived = std::min(
        {canBuyAtMost(sheep, sheepLineAccountB,
                      sellingWheatOffer.getPrice()),
         canSellAtMost(acocuntBFrame, wheat, wheatLineAccountB, ledgerManager),
         sellingWheatOffer.getAmount()});
    assert(numWheatReceived >= 0);
    sellingWheatOffer.setAmount(numWheatReceived);
    auto exchangeResult =
        ledgerManager.getCurrentLedgerVersion() < 3
            ? exchangeV2(numWheatReceived, sellingWheatOffer.getPrice(),
                         maxWheatReceived, maxSheepSend)
            : exchangeV3(numWheatReceived, sellingWheatOffer.getPrice(),
                         maxWheatReceived, maxSheepSend);

    numWheatReceived = exchangeResult.numWheatReceived;
    numSheepSend = exchangeResult.numSheepSend;

    bool offerTaken = false;

    switch (exchangeResult.type())
    {
    case ExchangeResultType::REDUCED_TO_ZERO:
        return eOfferCantConvert;
    case ExchangeResultType::BOGUS:
        // force delete the offer as it represents a bogus offer
        numWheatReceived = 0;
        numSheepSend = 0;
        offerTaken = true;
        break;
    default:
        break;
    }

    offerTaken =
        offerTaken || sellingWheatOffer.getAmount() <= numWheatReceived;
    if (offerTaken)
    { // entire offer is taken
        ledgerDelta.deleteEntry(sellingWheatOffer.getKey());

        acocuntBFrame.addNumEntries(-1, ledgerManager);
        ledgerDelta.updateEntry(acocuntBFrame);
    }
    else
    {
        sellingWheatOffer.setAmount(sellingWheatOffer.getAmount() - numWheatReceived);
        ledgerDelta.updateEntry(sellingWheatOffer);
    }

    // Adjust balances
    if (numSheepSend != 0)
    {
        if (sheep.type() == ASSET_TYPE_NATIVE)
        {
            if (!acocuntBFrame.addBalance(numSheepSend))
            {
                return eOfferCantConvert;
            }
            ledgerDelta.updateEntry(acocuntBFrame);
        }
        else
        {
            auto sheepTrust = TrustFrame{*sheepLineAccountB};
            if (!sheepTrust.addBalance(numSheepSend))
            {
                return eOfferCantConvert;
            }
            ledgerDelta.updateEntry(sheepTrust);
        }
    }

    if (numWheatReceived != 0)
    {
        if (wheat.type() == ASSET_TYPE_NATIVE)
        {
            acocuntBFrame.setBalance(acocuntBFrame.getBalance() - numWheatReceived);
            if (!acocuntBFrame.addBalance(-numWheatReceived))
            {
                return eOfferCantConvert;
            }
            ledgerDelta.updateEntry(acocuntBFrame);
        }
        else
        {
            auto wheatTrust = TrustFrame{*wheatLineAccountB};
            if (!wheatTrust.addBalance(-numWheatReceived))
            {
                return eOfferCantConvert;
            }
            ledgerDelta.updateEntry(wheatTrust);
        }
    }

    mOfferTrail.push_back(
        ClaimOfferAtom(acocuntBFrame.getAccountID(), sellingWheatOffer.getOfferID(), wheat,
                       numWheatReceived, sheep, numSheepSend));

    return offerTaken ? eOfferTaken : eOfferPartial;
}

OfferExchange::ConvertResult
OfferExchange::convertWithOffers(
    LedgerDelta &ledgerDelta,
    Asset const& sheep, int64_t maxSheepSend, int64_t& sheepSend,
    Asset const& wheat, int64_t maxWheatReceive, int64_t& wheatReceived,
    std::function<OfferFilterResult(LedgerEntry const&)> filter)
{
    sheepSend = 0;
    wheatReceived = 0;

    auto& ledgerManager = mApp.getLedgerManager();
    auto& db = ledgerManager.getDatabase();

    size_t offerOffset = 0;

    bool needMore = (maxWheatReceive > 0 && maxSheepSend > 0);

    while (needMore)
    {
        auto retList = selectBestOffers(5, offerOffset, wheat, sheep, db);
        offerOffset += retList.size();

        for (auto& wheatOffer : retList)
        {
            if (filter)
            {
                OfferFilterResult r = filter(wheatOffer);
                switch (r)
                {
                case eKeep:
                    break;
                case eStop:
                    return eFilterStop;
                case eSkip:
                    continue;
                }
            }

            int64_t numWheatReceived;
            int64_t numSheepSend;

            CrossOfferResult cor =
                crossOffer(ledgerDelta,
                           OfferFrame{wheatOffer}, maxWheatReceive, numWheatReceived,
                           maxSheepSend, numSheepSend);

            assert(numSheepSend >= 0);
            assert(numSheepSend <= maxSheepSend);
            assert(numWheatReceived >= 0);
            assert(numWheatReceived <= maxWheatReceive);

            switch (cor)
            {
            case eOfferTaken:
                assert(offerOffset > 0);
                offerOffset--; // adjust offset as an offer was deleted
                break;
            case eOfferPartial:
                break;
            case eOfferCantConvert:
                return ePartial;
            }

            sheepSend += numSheepSend;
            maxSheepSend -= numSheepSend;

            wheatReceived += numWheatReceived;
            maxWheatReceive -= numWheatReceived;

            needMore = (maxWheatReceive > 0 && maxSheepSend > 0);
            if (!needMore)
            {
                return eOK;
            }
            else if (cor == eOfferPartial)
            {
                return ePartial;
            }
        }

        // still stuff to fill but no more offers
        if (needMore && retList.size() < 5)
        {
            return eOK;
        }
    }
    return eOK;
}
}
