#pragma once

#include "transactions/TransactionFrame.h"
#include "ledger/OfferFrame.h"
#include <vector>
#include <functional>

namespace stellar
{

    class OfferExchange
    {

        /*
        // returns false if tx should be canceled
        bool convert(Currency& sell,
            Currency& buy, int64_t amountToBuy,
            int64_t& retAmountToSell,
            LedgerDelta& delta, LedgerMaster& ledgerMaster);
            */

        LedgerDelta &mDelta;
        LedgerMaster &mLedgerMaster;

        std::vector<ClaimOfferAtom> mOfferTrail;
    public:

        OfferExchange(LedgerDelta &delta, LedgerMaster &ledgerMaster);

        // buys from a single offer
        enum CrossOfferResult { eOfferPartial, eOfferTaken, eOfferCantConvert, eOfferError };
        CrossOfferResult crossOffer(OfferFrame& sellingWheatOffer,
            int64_t maxWheatReceived, int64_t& numWheatReceived, int64_t wheatTransferRate,
            int64_t maxSheepReceive, int64_t& numSheepSent, int64_t& numSheepReceived, int64_t sheepTransferRate);

        enum OfferFilterResult { eKeep, eSkip, eFail, eStop };

        enum ConvertResult { eOK, eFilterFail, eFilterStop, eBadOffer, eNotEnoughOffers };
        // buys wheat with sheep, crossing as many offers as necessary
        ConvertResult convertWithOffers(
            Currency& sheep, int64_t maxSheepReceive, int64_t &sheepReceived, int64_t &sheepSend,
            Currency& wheat, int64_t maxWheatReceive, int64_t &weatReceived, std::function<OfferFilterResult(const OfferFrame &)> filter);

        std::vector<ClaimOfferAtom> getOfferTrail() { return mOfferTrail; }
    };

}
