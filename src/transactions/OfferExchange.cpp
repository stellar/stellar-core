// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "OfferExchange.h"
#include "ledger/LedgerManager.h"
#include "ledger/TrustFrame.h"
#include "database/Database.h"
#include "util/Logging.h"
#include "ledger/LedgerDelta.h"

namespace stellar
{

namespace
{

int64_t
canBuyAtMost(const Asset &asset, TrustFrame::pointer trustLine, Price &price)
{
    if (asset.type() == ASSET_TYPE_NATIVE)
    {
        return INT64_MAX;
    }

    // compute value based on what the account can receive
    auto sellerMaxSheep =
        trustLine ? trustLine->getMaxAmountReceive() : 0;

    auto result = int64_t{};
    if (!bigDivide(result, sellerMaxSheep, price.d, price.n))
    {
        result = INT64_MAX;
    }

    return result;
}

int64_t
canSellAtMost(AccountFrame::pointer account, const Asset &asset, TrustFrame::pointer trustLine, LedgerManager &ledgerManager)
{
    int64_t result;
    if (asset.type() == ASSET_TYPE_NATIVE)
    {
        // can only send above the minimum balance
        return account->getBalanceAboveReserve(ledgerManager);
    }

    if (trustLine && trustLine->isAuthorized())
    {
        return trustLine->getBalance();
    }

    return 0;
}

}

ExchangeResult
exchange(int64_t wheatReceived, Price price, int64_t maxWheatReceive, int64_t maxSheepSend)
{
    auto result = ExchangeResult{};
    result.reduced = wheatReceived > maxWheatReceive;
    wheatReceived = std::min(wheatReceived, maxWheatReceive);

    // this guy can get X wheat to you. How many sheep does that get him?
    if (!bigDivide(result.numSheepSend, wheatReceived, price.n, price.d))
    {
        result.numSheepSend = INT64_MAX;
    }

    result.reduced = result.reduced || (result.numSheepSend > maxSheepSend);
    result.numSheepSend = std::min(result.numSheepSend, maxSheepSend);
    // bias towards seller (this cannot overflow at this point)
    result.numWheatReceived = bigDivide(result.numSheepSend, price.d, price.n);

    return result;
}

OfferExchange::OfferExchange(LedgerDelta& delta, LedgerManager& ledgerManager)
    : mDelta(delta), mLedgerManager(ledgerManager)
{
}

OfferExchange::CrossOfferResult
OfferExchange::crossOffer(OfferFrame& sellingWheatOffer,
                          int64_t maxWheatReceived, int64_t& numWheatReceived,
                          int64_t maxSheepSend, int64_t& numSheepSend)
{
    // we're about to make changes to the offer
    mDelta.recordEntry(sellingWheatOffer);

    Asset& sheep = sellingWheatOffer.getOffer().buying;
    Asset& wheat = sellingWheatOffer.getOffer().selling;
    AccountID& accountBID = sellingWheatOffer.getOffer().sellerID;

    Database& db = mLedgerManager.getDatabase();

    AccountFrame::pointer accountB;
    accountB = AccountFrame::loadAccount(mDelta, accountBID, db);
    if (!accountB)
    {
        throw std::runtime_error(
            "invalid database state: offer must have matching account");
    }

    TrustFrame::pointer wheatLineAccountB;
    if (wheat.type() != ASSET_TYPE_NATIVE)
    {
        wheatLineAccountB =
            TrustFrame::loadTrustLine(accountBID, wheat, db, &mDelta);
    }

    TrustFrame::pointer sheepLineAccountB;
    if (sheep.type() != ASSET_TYPE_NATIVE)
    {
        sheepLineAccountB =
            TrustFrame::loadTrustLine(accountBID, sheep, db, &mDelta);
    }

    numWheatReceived = std::min({
        canBuyAtMost(sheep, sheepLineAccountB, sellingWheatOffer.getOffer().price),
        canSellAtMost(accountB, wheat, wheatLineAccountB, mLedgerManager),
        sellingWheatOffer.getOffer().amount
    });
    sellingWheatOffer.getOffer().amount = numWheatReceived;
    auto exchangeResult = exchange(numWheatReceived, sellingWheatOffer.getOffer().price, maxWheatReceived, maxSheepSend);
    numWheatReceived = exchangeResult.numWheatReceived;
    numSheepSend = exchangeResult.numSheepSend;

    bool offerTaken = false;

    if (numWheatReceived == 0 || numSheepSend == 0)
    {
        if (exchangeResult.reduced)
        {
            return eOfferCantConvert;
        }
        else
        {
            // force delete the offer as it represents a bogus offer
            numWheatReceived = 0;
            numSheepSend = 0;
            offerTaken = true;
        }
    }

    offerTaken =
        offerTaken || sellingWheatOffer.getOffer().amount <= numWheatReceived;
    if (offerTaken)
    { // entire offer is taken
        sellingWheatOffer.storeDelete(mDelta, db);

        accountB->addNumEntries(-1, mLedgerManager);
        accountB->storeChange(mDelta, db);
    }
    else
    {
        sellingWheatOffer.getOffer().amount -= numWheatReceived;
        sellingWheatOffer.storeChange(mDelta, db);
    }

    // Adjust balances
    if (numSheepSend != 0)
    {
        if (sheep.type() == ASSET_TYPE_NATIVE)
        {
            accountB->getAccount().balance += numSheepSend;
            accountB->storeChange(mDelta, db);
        }
        else
        {
            if (!sheepLineAccountB->addBalance(numSheepSend))
            {
                return eOfferCantConvert;
            }
            sheepLineAccountB->storeChange(mDelta, db);
        }
    }

    if (numWheatReceived != 0)
    {
        if (wheat.type() == ASSET_TYPE_NATIVE)
        {
            accountB->getAccount().balance -= numWheatReceived;
            accountB->storeChange(mDelta, db);
        }
        else
        {
            if (!wheatLineAccountB->addBalance(-numWheatReceived))
            {
                return eOfferCantConvert;
            }
            wheatLineAccountB->storeChange(mDelta, db);
        }
    }

    mOfferTrail.push_back(
        ClaimOfferAtom(accountB->getID(), sellingWheatOffer.getOfferID(), wheat,
                       numWheatReceived, sheep, numSheepSend));

    return offerTaken ? eOfferTaken : eOfferPartial;
}

OfferExchange::ConvertResult
OfferExchange::convertWithOffers(
    Asset const& sheep, int64_t maxSheepSend, int64_t& sheepSend,
    Asset const& wheat, int64_t maxWheatReceive, int64_t& wheatReceived,
    std::function<OfferFilterResult(OfferFrame const&)> filter)
{
    sheepSend = 0;
    wheatReceived = 0;

    Database& db = mLedgerManager.getDatabase();

    size_t offerOffset = 0;

    bool needMore = (maxWheatReceive > 0 && maxSheepSend > 0);

    while (needMore)
    {
        std::vector<OfferFrame::pointer> retList;
        OfferFrame::loadBestOffers(5, offerOffset, wheat, sheep, retList, db);

        offerOffset += retList.size();

        for (auto& wheatOffer : retList)
        {
            if (filter)
            {
                OfferFilterResult r = filter(*wheatOffer);
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
                crossOffer(*wheatOffer, maxWheatReceive, numWheatReceived,
                           maxSheepSend, numSheepSend);

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
