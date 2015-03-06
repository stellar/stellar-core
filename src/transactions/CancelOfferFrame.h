#pragma once

#include "transactions/TransactionFrame.h"

namespace stellar
{
    class CancelOfferFrame : public OperationFrame
    {
        CancelOffer::CancelOfferResult &innerResult() { return mResult.tr().cancelOfferResult(); }
    public:
        CancelOfferFrame(Operation const& op, OperationResult &res, TransactionFrame &parentTx);

        bool doApply(LedgerDelta& delta, LedgerMaster& ledgerMaster);
        bool doCheckValid(Application& app);
    };

    namespace CancelOffer
    {
        inline CancelOffer::CancelOfferResultCode getInnerCode(OperationResult const & res)
        {
            return res.tr().cancelOfferResult().code();
        }
    }

}