// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "OfferExchange.h"
#include "database/Database.h"
#include "ledger/LedgerEntryReference.h"
#include "ledger/LedgerHeaderReference.h"
#include "ledger/LedgerState.h"
#include "util/Logging.h"
#include "transactions/TransactionUtils.h"
#include <memory>

#include "ledger/AccountReference.h"
#include "ledger/OfferReference.h"
#include "ledger/TrustLineReference.h"

namespace stellar
{

namespace
{

int64_t
canBuyAtMost(Asset const& asset, std::shared_ptr<TrustLineReference> trustLine, Price& price)
{
    if (asset.type() == ASSET_TYPE_NATIVE)
    {
        return INT64_MAX;
    }

    // compute value based on what the account can receive
    auto sellerMaxSheep = trustLine ? trustLine->getMaxAmountReceive() : 0;

    auto result = int64_t{};
    if (!bigDivide(result, sellerMaxSheep, price.d, price.n, ROUND_DOWN))
    {
        result = INT64_MAX;
    }

    return result;
}

int64_t
canSellAtMost(AccountReference account, Asset const& asset,
              std::shared_ptr<TrustLineReference> trustLine,
              std::shared_ptr<LedgerHeaderReference> header)
{
    if (asset.type() == ASSET_TYPE_NATIVE)
    {
        // can only send above the minimum balance
        return account.getBalanceAboveReserve(header);
    }

    if (trustLine && trustLine->isAuthorized())
    {
        return trustLine->getBalance();
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

OfferExchange::CrossOfferResult
OfferExchange::crossOffer(
    LedgerState& ls, std::shared_ptr<LedgerEntryReference> sellingWheatOffer,
    int64_t maxWheatReceived, int64_t& numWheatReceived,
    int64_t maxSheepSend, int64_t& numSheepSend)
{
    assert(maxWheatReceived > 0);
    assert(maxSheepSend > 0);

    // Note: These must be copies, not references.
    Asset sheep = sellingWheatOffer->entry()->data.offer().buying;
    Asset wheat = sellingWheatOffer->entry()->data.offer().selling;
    AccountID accountBID = sellingWheatOffer->entry()->data.offer().sellerID;

    {
        auto accountB = stellar::loadAccount(ls, accountBID);
        if (!accountB)
        {
            throw std::runtime_error(
                "invalid database state: offer must have matching account");
        }

        std::shared_ptr<TrustLineReference> wheatLineAccountB;
        if (wheat.type() != ASSET_TYPE_NATIVE)
        {
            wheatLineAccountB = loadTrustLine(ls, accountBID, wheat);
        }

        std::shared_ptr<TrustLineReference> sheepLineAccountB;
        if (sheep.type() != ASSET_TYPE_NATIVE)
        {
            sheepLineAccountB = loadTrustLine(ls, accountBID, sheep);
        }

        auto header = ls.loadHeader();
        numWheatReceived = std::min(
            {canBuyAtMost(sheep, sheepLineAccountB,
                          sellingWheatOffer->entry()->data.offer().price),
             canSellAtMost(accountB, wheat, wheatLineAccountB, header),
             sellingWheatOffer->entry()->data.offer().amount});
        assert(numWheatReceived >= 0);
        header->invalidate();

        if (accountB)
        {
            accountB.forget(ls);
        }
        if (wheatLineAccountB)
        {
            wheatLineAccountB->forget(ls);
        }
        if (sheepLineAccountB)
        {
            sheepLineAccountB->forget(ls);
        }
    }

    sellingWheatOffer->entry()->data.offer().amount = numWheatReceived;
    auto header = ls.loadHeader();
    auto exchangeResult =
        getCurrentLedgerVersion(header) < 3
            ? exchangeV2(numWheatReceived, sellingWheatOffer->entry()->data.offer().price,
                         maxWheatReceived, maxSheepSend)
            : exchangeV3(numWheatReceived, sellingWheatOffer->entry()->data.offer().price,
                         maxWheatReceived, maxSheepSend);
    header->invalidate();

    numWheatReceived = exchangeResult.numWheatReceived;
    numSheepSend = exchangeResult.numSheepSend;

    bool offerTaken = false;

    switch (exchangeResult.type())
    {
    case ExchangeResultType::REDUCED_TO_ZERO:
        ls.forget(sellingWheatOffer);
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

    auto offerID = sellingWheatOffer->entry()->data.offer().offerID;

    offerTaken =
        offerTaken || sellingWheatOffer->entry()->data.offer().amount <= numWheatReceived;
    if (offerTaken)
    { // entire offer is taken
        sellingWheatOffer->erase();

        auto accountB = stellar::loadAccount(ls, accountBID);
        auto header = ls.loadHeader();
        accountB.addNumEntries(header, -1);
        header->invalidate();
        accountB.invalidate();
    }
    else
    {
        sellingWheatOffer->entry()->data.offer().amount -= numWheatReceived;
    }

    // Adjust balances
    if (numSheepSend != 0)
    {
        // This ledger state is to make it so that we don't record that we
        // loaded accountB or sheepLineAccountB if we return eOfferCantConvert.
        // We need a LedgerState here since it is possible that changes to
        // accountB were already stored.
        LedgerState lsInner(ls);
        if (sheep.type() == ASSET_TYPE_NATIVE)
        {
            AccountReference accountB = stellar::loadAccount(lsInner, accountBID);
            if (!accountB.addBalance(numSheepSend))
            {
                return eOfferCantConvert;
            }
        }
        else
        {
            auto sheepLineAccountB = loadTrustLine(lsInner, accountBID, sheep);
            if (!sheepLineAccountB->addBalance(numSheepSend))
            {
                return eOfferCantConvert;
            }
        }
        lsInner.commit();
    }

    if (numWheatReceived != 0)
    {
        // This ledger state is to make it so that we don't record that we
        // loaded accountB or wheatLineAccountB if we return eOfferCantConvert.
        // We need a LedgerState here since it is possible that changes to
        // accountB were already stored.
        LedgerState lsInner(ls);
        if (wheat.type() == ASSET_TYPE_NATIVE)
        {
            AccountReference accountB = stellar::loadAccount(lsInner, accountBID);
            if (!accountB.addBalance(-numWheatReceived))
            {
                return eOfferCantConvert;
            }
        }
        else
        {
            auto wheatLineAccountB = loadTrustLine(lsInner, accountBID, wheat);
            if (!wheatLineAccountB->addBalance(-numWheatReceived))
            {
                return eOfferCantConvert;
            }
        }
        lsInner.commit();
    }

    mOfferTrail.push_back(
        ClaimOfferAtom(accountBID, offerID, wheat,
                       numWheatReceived, sheep, numSheepSend));

    return offerTaken ? eOfferTaken : eOfferPartial;
}

OfferExchange::ConvertResult
OfferExchange::convertWithOffers(
    LedgerState& ls,
    Asset const& sheep, int64_t maxSheepSend, int64_t& sheepSend,
    Asset const& wheat, int64_t maxWheatReceive, int64_t& wheatReceived,
    std::function<OfferFilterResult(OfferReference)> filter)
{
    sheepSend = 0;
    wheatReceived = 0;

    bool needMore = (maxWheatReceive > 0 && maxSheepSend > 0);
    while (needMore)
    {
        LedgerState lsInner(ls);
        //auto wheatOffer = lsInner.loadBestOffer(sheep, wheat);
        auto wheatOffer = lsInner.loadBestOffer(wheat, sheep);
        if (!wheatOffer)
        {
            break;
        }
        if (filter && filter(wheatOffer) == eStop)
        {
            return eFilterStop;
        }

        int64_t numWheatReceived;
        int64_t numSheepSend;

        CrossOfferResult cor =
            crossOffer(lsInner, wheatOffer, maxWheatReceive, numWheatReceived,
                       maxSheepSend, numSheepSend);
        lsInner.commit();

        assert(numSheepSend >= 0);
        assert(numSheepSend <= maxSheepSend);
        assert(numWheatReceived >= 0);
        assert(numWheatReceived <= maxWheatReceive);

        if (cor == eOfferCantConvert)
        {
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
    return eOK;
}
}
