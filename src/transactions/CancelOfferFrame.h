#pragma once

#include "transactions/TransactionFrame.h"

namespace stellar
{
    class CancelOfferFrame : public TransactionFrame
    {
        CancelOffer::CancelOfferResult &innerResult() { return mResult.body.tr().cancelOfferResult(); }
    public:
        CancelOfferFrame(const TransactionEnvelope& envelope);

        bool doApply(LedgerDelta& delta, LedgerMaster& ledgerMaster);
        bool doCheckValid(Application& app);
    };

    namespace CancelOffer
    {
        inline CancelOffer::CancelOfferResultCode getInnerCode(TransactionResult const & res)
        {
            return res.body.tr().cancelOfferResult().result.code();
        }
    }

}