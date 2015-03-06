#pragma once

#include "transactions/TransactionFrame.h"
#include "ledger/TrustFrame.h"
#include "ledger/OfferFrame.h"

namespace stellar
{
    class CreateOfferFrame : public OperationFrame
    {
        TrustFrame mSheepLineA;
        TrustFrame mWheatLineA;

        OfferFrame mSellSheepOffer;

        bool checkOfferValid(Database& db); 
        
        CreateOffer::CreateOfferResult &innerResult() { return mResult.tr().createOfferResult(); }

        CreateOfferOp const& mCreateOffer;
    public:
        CreateOfferFrame(Operation const& op, OperationResult &res, TransactionFrame &parentTx);

        bool doApply(LedgerDelta& delta, LedgerMaster& ledgerMaster);
        bool doCheckValid(Application& app);
    };

    namespace CreateOffer
    {
        inline CreateOffer::CreateOfferResultCode getInnerCode(OperationResult const & res)
        {
            return res.tr().createOfferResult().code();
        }
    }

}