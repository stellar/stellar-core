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
        int64_t maxWheatReceived, int64_t& numWheatReceived, int64_t wheatTransferRate,
        int64_t maxSheepReceive, int64_t& numSheepSent, int64_t& numSheepReceived, int64_t sheepTransferRate)
    {
        Currency& sheep = sellingWheatOffer.mEntry.offer().takerPays;
        Currency& wheat = sellingWheatOffer.mEntry.offer().takerGets;
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

        if (wheatTransferRate != TRANSFER_RATE_DIVISOR)
        {
            // adjust available balance by the rate
            numWheatReceived = bigDivide(numWheatReceived, wheatTransferRate, TRANSFER_RATE_DIVISOR);
        }

        // you can receive the lesser of the amount of wheat offered or
        // the amount the guy has
        if (numWheatReceived >= sellingWheatOffer.mEntry.offer().amount)
        {
            numWheatReceived = sellingWheatOffer.mEntry.offer().amount;
        }
        else
        {
            // update the offer based on the balance (to determine if it should be deleted or not)
            // note that we don't need to write into the db at this point as the actual update
            // is done further down
            sellingWheatOffer.mEntry.offer().amount = numWheatReceived;
        }

        if (numWheatReceived > maxWheatReceived)
        {
            numWheatReceived = maxWheatReceived;
        }

        // TODO: there are a bunch of rounding modes here
        // this should be simplified as it may create situations where an offer
        // can never be completely taken and sticks around (for example)

        // this guy can get X wheat to you. How many sheep does that get him?
        numSheepReceived = bigDivide(numWheatReceived, sellingWheatOffer.mEntry.offer().price, OFFER_PRICE_DIVISOR);
        numSheepSent = bigDivide(numSheepReceived, TRANSFER_RATE_DIVISOR, sheepTransferRate);

        if (numSheepReceived > maxSheepReceive)
        {
            // reduce the number even more if there is a limit on Sheep
            numWheatReceived = bigDivide(maxSheepReceive, OFFER_PRICE_DIVISOR, sellingWheatOffer.mEntry.offer().price);

            numSheepReceived = bigDivide(numWheatReceived, sellingWheatOffer.mEntry.offer().price, OFFER_PRICE_DIVISOR);
            numSheepSent = bigDivide(numSheepReceived, TRANSFER_RATE_DIVISOR, sheepTransferRate);
            assert(numSheepReceived <= maxSheepReceive);
        }

        // computes the amount leaving the source account
        int64_t numWheatSent;
        if (wheatTransferRate != TRANSFER_RATE_DIVISOR)
        {
            numWheatSent = bigDivide(numWheatReceived, TRANSFER_RATE_DIVISOR, wheatTransferRate); \
        }
        else
        {
            numWheatSent = numWheatReceived;
        }

        if (numWheatReceived == 0 || numSheepReceived == 0)
        {
            // TODO: cleanup offers that result in this (offer amount too low to be converted)
            return eOfferCantConvert;
        }

        bool offerTaken = sellingWheatOffer.mEntry.offer().amount <= numWheatReceived;
        if (offerTaken)
        {   // entire offer is taken
            sellingWheatOffer.storeDelete(mDelta, db);
            accountB.mEntry.account().ownerCount--;
            accountB.storeChange(mDelta, db);
        }
        else
        {
            sellingWheatOffer.mEntry.offer().amount -= numWheatReceived;
            sellingWheatOffer.storeChange(mDelta, db);
        }

        // Adjust balances
        if (sheep.type() == NATIVE)
        {
            accountB.mEntry.account().balance += numSheepReceived;
            accountB.storeChange(mDelta, db);
        }
        else
        {
            sheepLineAccountB.mEntry.trustLine().balance += numSheepReceived;
            sheepLineAccountB.storeChange(mDelta, db);
        }

        if (wheat.type() == NATIVE)
        {
            accountB.mEntry.account().balance -= numWheatSent;
            accountB.storeChange(mDelta, db);
        }
        else
        {
            wheatLineAccountB.mEntry.trustLine().balance -= numWheatSent;
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
        Currency& sheep, int64_t maxSheepReceive, int64_t &sheepReceived, int64_t &sheepSend,
        Currency& wheat, int64_t maxWheatReceive, int64_t &wheatReceived,
        std::function<OfferFilterResult(const OfferFrame &)> filter)
    {
        int64_t wheatTransferRate = TransactionFrame::getTransferRate(wheat, mLedgerMaster);
        int64_t sheepTransferRate = TransactionFrame::getTransferRate(sheep, mLedgerMaster);

        sheepReceived = 0;
        sheepSend = 0;
        wheatReceived = 0;

        Database &db = mLedgerMaster.getDatabase();

        size_t offerOffset = 0;

        while (maxWheatReceive > 0 && maxSheepReceive > 0)
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
                int64_t numSheepSend, numSheepReceived;

                CrossOfferResult cor = crossOffer(wheatOffer,
                    maxWheatReceive, numWheatReceived, wheatTransferRate,
                    maxSheepReceive, numSheepSend, numSheepReceived, sheepTransferRate);

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

                sheepReceived += numSheepReceived;
                sheepSend += numSheepSend;
                maxSheepReceive -= numSheepReceived;

                wheatReceived += numWheatReceived;
                maxWheatReceive -= numWheatReceived;
            }
            // still stuff to fill but no more offers
            if ((maxWheatReceive > 0 || maxSheepReceive) && retList.size() < 5)
            { // there isn't enough offer depth
                // TODO: replace bool with a OfferReturnCode (namespace to share with atom)
                // innerResult().result.code(Payment::OVERSENDMAX);
                return eNotEnoughOffers;
            }
            offerOffset += retList.size();
        }
        return eOK;
    }
}
