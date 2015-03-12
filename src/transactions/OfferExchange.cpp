// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "OfferExchange.h"
#include "ledger/LedgerMaster.h"
#include "ledger/TrustFrame.h"
#include "database/Database.h"
#include "util/Logging.h"

namespace stellar
{

    OfferExchange::OfferExchange(LedgerDelta &delta, LedgerMaster &ledgerMaster) :
        mDelta(delta), mLedgerMaster(ledgerMaster)
    {

    }

    OfferExchange::CrossOfferResult OfferExchange::crossOffer(OfferFrame& sellingWheatOffer,
        int64_t maxWheatReceived, int64_t& numWheatReceived,
        int64_t maxSheepSend, int64_t& numSheepSend)
    {
        Currency& sheep = sellingWheatOffer.getOffer().takerPays;
        Currency& wheat = sellingWheatOffer.getOffer().takerGets;
        uint256& accountBID = sellingWheatOffer.getOffer().accountID;

        Database &db = mLedgerMaster.getDatabase();

        AccountFrame accountB;
        if (!AccountFrame::loadAccount(accountBID, accountB, db))
        {
            throw std::runtime_error("invalid database state: offer must have matching account");
        }

        TrustFrame wheatLineAccountB;
        if (wheat.type() != NATIVE)
        {
            if (!TrustFrame::loadTrustLine(accountBID,
                wheat, wheatLineAccountB, db))
            {
                throw std::runtime_error("invalid database state: offer must have matching trust line");
            }
        }

        TrustFrame sheepLineAccountB;
        if (sheep.type() != NATIVE)
        {
            if (!TrustFrame::loadTrustLine(accountBID,
                sheep, sheepLineAccountB, db))
            {
                throw std::runtime_error("invalid database state: offer must have matching trust line");
            }
        }

        // what the seller has
        if (wheat.type() == NATIVE)
        {
            // can only send above the minimum balance
            numWheatReceived = accountB.getAccount().balance-accountB.getMinimumBalance(mLedgerMaster);
            if (numWheatReceived < 0)
            {
                numWheatReceived = 0;
            }
        }
        else
        {
            numWheatReceived = wheatLineAccountB.getTrustLine().balance;
        }

        // you can receive the lesser of the amount of wheat offered or
        // the amount the guy has

        if (numWheatReceived >= sellingWheatOffer.getOffer().amount)
        {
            numWheatReceived = sellingWheatOffer.getOffer().amount;
        }
        else
        {
            // update the offer based on the balance (to determine if it should be deleted or not)
            // note that we don't need to write into the db at this point as the actual update
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
        numSheepSend = bigDivide(
            numWheatReceived, sellingWheatOffer.getOffer().price.n,
            sellingWheatOffer.getOffer().price.d);

        if (numSheepSend > maxSheepSend)
        {
            // reduce the number even more if there is a limit on Sheep
            numSheepSend = maxSheepSend;
            reducedOffer = true;
        }

        // bias towards seller
        numWheatReceived = bigDivide(
            numSheepSend, sellingWheatOffer.getOffer().price.d,
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

        offerTaken = offerTaken || sellingWheatOffer.getOffer().amount <= numWheatReceived;
        if (offerTaken)
        {   // entire offer is taken
            sellingWheatOffer.storeDelete(mDelta, db);
            accountB.getAccount().numSubEntries--;
            accountB.storeChange(mDelta, db);
        }
        else
        {
            sellingWheatOffer.getOffer().amount -= numWheatReceived;
            sellingWheatOffer.storeChange(mDelta, db);
        }

        // Adjust balances
        if (sheep.type() == NATIVE)
        {
            accountB.getAccount().balance += numSheepSend;
            accountB.storeChange(mDelta, db);
        }
        else
        {
            sheepLineAccountB.getTrustLine().balance += numSheepSend;
            sheepLineAccountB.storeChange(mDelta, db);
        }

        if (wheat.type() == NATIVE)
        {
            accountB.getAccount().balance -= numWheatReceived;
            accountB.storeChange(mDelta, db);
        }
        else
        {
            wheatLineAccountB.getTrustLine().balance -= numWheatReceived;
            wheatLineAccountB.storeChange(mDelta, db);
        }

        mOfferTrail.push_back(ClaimOfferAtom(
            accountB.getID(),
            sellingWheatOffer.getOfferID(),
            wheat,
            numWheatReceived
            ));

        return offerTaken ? eOfferTaken : eOfferPartial;
    }

    OfferExchange::ConvertResult OfferExchange::convertWithOffers(
        Currency const& sheep, int64_t maxSheepSend, int64_t &sheepSend,
        Currency const& wheat, int64_t maxWheatReceive, int64_t &wheatReceived,
        std::function<OfferFilterResult(OfferFrame const&)> filter)
    {
        sheepSend = 0;
        wheatReceived = 0;

        Database &db = mLedgerMaster.getDatabase();

        size_t offerOffset = 0;

        bool needMore = (maxWheatReceive > 0 && maxSheepSend > 0);

        while (needMore)
        {
            std::vector<OfferFrame> retList;
            OfferFrame::loadBestOffers(5, offerOffset, sheep, wheat, retList, db);

            offerOffset += retList.size();

            for (auto wheatOffer : retList)
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
                    }
                }

                int64_t numWheatReceived;
                int64_t numSheepSend;

                CrossOfferResult cor = crossOffer(wheatOffer,
                    maxWheatReceive, numWheatReceived,
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
