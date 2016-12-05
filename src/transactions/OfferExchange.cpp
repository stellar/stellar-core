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

    numWheatReceived = canBuyAtMost(sheep, sheepLineAccountB, sellingWheatOffer.getOffer().price);

    // adjust numWheatReceived with what the seller has
    auto wheatCanSell = canSellAtMost(accountB, wheat, wheatLineAccountB, mLedgerManager);
    if (numWheatReceived > wheatCanSell)
    {
        numWheatReceived = wheatCanSell;
    }

    // you can receive the lesser of the amount of wheat offered or
    // the amount the guy has

    if (numWheatReceived >= sellingWheatOffer.getOffer().amount)
    {
        numWheatReceived = sellingWheatOffer.getOffer().amount;
    }
    else
    {
        // update the offer based on the balance (to determine if it should be
        // deleted or not)
        // note that we don't need to write into the db at this point as the
        // actual update
        // is done further down
        sellingWheatOffer.getOffer().amount = numWheatReceived;
    }

    bool reducedOffer = false;

    if (numWheatReceived > maxWheatReceived)
    {
        numWheatReceived = maxWheatReceived;
        reducedOffer = true;
    }

    // this guy can get X wheat to you. How many sheep does that get him?
    if (!bigDivide(numSheepSend, numWheatReceived,
                   sellingWheatOffer.getOffer().price.n,
                   sellingWheatOffer.getOffer().price.d))
    {
        numSheepSend = INT64_MAX;
    }

    if (numSheepSend > maxSheepSend)
    {
        // reduce the number even more if there is a limit on Sheep
        numSheepSend = maxSheepSend;
        reducedOffer = true;
    }

    // bias towards seller (this cannot overflow at this point)
    numWheatReceived =
        bigDivide(numSheepSend, sellingWheatOffer.getOffer().price.d,
                  sellingWheatOffer.getOffer().price.n);

    bool offerTaken = false;

    if (numWheatReceived == 0 || numSheepSend == 0)
    {
        if (reducedOffer)
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
