#pragma once

#include "transactions/TransactionFrame.h"
#include "ledger/TrustFrame.h"
#include "ledger/OfferFrame.h"

namespace stellar
{
    class CreateOfferFrame : public TransactionFrame
    {
        TrustFrame mSheepLineA;
        TrustFrame mWheatLineA;

        int64_t mWheatTransferRate;
        int64_t mSheepTransferRate;

        OfferFrame mSellSheepOffer;

        bool checkOfferValid(Database& db); 
        
        CreateOffer::CreateOfferResult &innerResult() { return mResult.body.tr().createOfferResult(); }
    public:
        CreateOfferFrame(const TransactionEnvelope& envelope);

        bool doApply(LedgerDelta& delta, LedgerMaster& ledgerMaster);
        bool doCheckValid(Application& app);
    };

    namespace CreateOffer
    {
        inline CreateOffer::CreateOfferResultCode getInnerCode(TransactionResult const & res)
        {
            return res.body.tr().createOfferResult().result.code();
        }
    }

}