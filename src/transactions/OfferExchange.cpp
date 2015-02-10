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
        Currency& sheep = sellingWheatOffer.mEntry.offer().buy;
        Currency& wheat = sellingWheatOffer.mEntry.offer().sell;
        uint256& accountBID = sellingWheatOffer.mEntry.offer().accountID;

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
            numWheatReceived = accountB.mEntry.account().balance;
        }
        else
        {
            numWheatReceived = wheatLineAccountB.mEntry.trustLine().balance;
        }

        // you can receive the lesser of the amount of wheat offered or
        // the amount the guy has

        if (numWheatReceived > maxWheatReceived)
        {
            numWheatReceived = maxWheatReceived;
        }

        if (numWheatReceived >= sellingWheatOffer.mEntry.offer().amountSell)
        {
            numWheatReceived = sellingWheatOffer.getSellAmount();
        }
        else
        {
            // update the offer based on the balance (to determine if it should be deleted or not)
            // note that we don't need to write into the db at this point as the actual update
            // is done further down
            sellingWheatOffer.adjustSellAmount(numWheatReceived);
        }

        // this guy can get X wheat to you. How many sheep does that get him?
        numSheepSend = sellingWheatOffer.getBuyAmount();

        if (numSheepSend > maxSheepSend)
        {
            numSheepSend = maxSheepSend;
        }
        // bias towards seller
        numWheatReceived = bigDivide(
            numSheepSend, sellingWheatOffer.getSellAmount(),
            sellingWheatOffer.getBuyAmount());

        if (numWheatReceived == 0 || numSheepSend == 0)
        {
            // TODO: cleanup offers that result in this (offer amount too low to be converted)
            return eOfferCantConvert;
        }

        bool offerTaken = sellingWheatOffer.getSellAmount() <= numWheatReceived ||
            sellingWheatOffer.getBuyAmount() <= numSheepSend;
        if (offerTaken)
        {   // entire offer is taken
            sellingWheatOffer.storeDelete(mDelta, db);
            accountB.mEntry.account().ownerCount--;
            accountB.storeChange(mDelta, db);
        }
        else
        {
            sellingWheatOffer.mEntry.offer().amountSell -= numWheatReceived;
            sellingWheatOffer.mEntry.offer().amountBuy -= numSheepSend;
            sellingWheatOffer.storeChange(mDelta, db);
        }

        // Adjust balances
        if (sheep.type() == NATIVE)
        {
            accountB.mEntry.account().balance += numSheepSend;
            accountB.storeChange(mDelta, db);
        }
        else
        {
            sheepLineAccountB.mEntry.trustLine().balance += numSheepSend;
            sheepLineAccountB.storeChange(mDelta, db);
        }

        if (wheat.type() == NATIVE)
        {
            accountB.mEntry.account().balance -= numWheatReceived;
            accountB.storeChange(mDelta, db);
        }
        else
        {
            wheatLineAccountB.mEntry.trustLine().balance -= numWheatReceived;
            wheatLineAccountB.storeChange(mDelta, db);
        }

        mOfferTrail.push_back(ClaimOfferAtom(
            accountB.getID(),
            sellingWheatOffer.getSequence(),
            wheat,
            numWheatReceived
            ));

        return offerTaken ? eOfferTaken : eOfferPartial;
    }

    OfferExchange::ConvertResult OfferExchange::convertWithOffers(
        Currency& sheep, int64_t maxSheepSend, int64_t &sheepSend,
        Currency& wheat, int64_t maxWheatReceive, int64_t &wheatReceived,
        std::function<OfferFilterResult(const OfferFrame &)> filter)
    {
        sheepSend = 0;
        wheatReceived = 0;

        Database &db = mLedgerMaster.getDatabase();

        size_t offerOffset = 0;

        while (maxWheatReceive > 0 && maxSheepSend > 0)
        {
            std::vector<OfferFrame> retList;
            OfferFrame::loadBestOffers(5, offerOffset, sheep, wheat, retList, db);
            for (auto wheatOffer : retList)
            {
                if (filter)
                {
                    OfferFilterResult r = filter(wheatOffer);
                    switch (r)
                    {
                    case eKeep:
                        break;
                    case eSkip:
                        continue;
                    case eFail:
                        return eFilterFail;
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
                case eOfferError:
                    return eBadOffer;
                case eOfferTaken:
                    offerOffset--; // adjust offset as an offer was deleted
                    assert(offerOffset >= 0);
                    break;
                case eOfferPartial:
                    break;
                case eOfferCantConvert:
                    return eOK;
                }

                sheepSend += numSheepSend;
                maxSheepSend -= numSheepSend;

                wheatReceived += numWheatReceived;
                maxWheatReceive -= numWheatReceived;
            }
            // still stuff to fill but no more offers
            if ((maxWheatReceive > 0 || (maxSheepSend != 0)) && retList.size() < 5)
            { // there isn't enough offer depth
                return eNotEnoughOffers;
            }
            offerOffset += retList.size();
        }
        return eOK;
    }
}
