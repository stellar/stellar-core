#pragma once

#include "transactions/OperationFrame.h"

namespace stellar
{
class CancelOfferOpFrame : public OperationFrame
{
    CancelOffer::CancelOfferResult&
    innerResult()
    {
        return mResult.tr().cancelOfferResult();
    }

  public:
    CancelOfferOpFrame(Operation const& op, OperationResult& res,
                       TransactionFrame& parentTx);

    bool doApply(LedgerDelta& delta, LedgerMaster& ledgerMaster);
    bool doCheckValid(Application& app);
};

namespace CancelOffer
{
inline CancelOffer::CancelOfferResultCode
getInnerCode(OperationResult const& res)
{
    return res.tr().cancelOfferResult().code();
}
}
}