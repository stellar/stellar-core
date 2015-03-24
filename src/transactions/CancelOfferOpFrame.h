#pragma once

#include "transactions/OperationFrame.h"

namespace stellar
{
class CancelOfferOpFrame : public OperationFrame
{
    CancelOfferResult&
    innerResult()
    {
        return mResult.tr().cancelOfferResult();
    }

  public:
    CancelOfferOpFrame(Operation const& op, OperationResult& res,
                       TransactionFrame& parentTx);

    bool doApply(LedgerDelta& delta, LedgerManager& ledgerManager);
    bool doCheckValid(Application& app);

    static CancelOfferResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().cancelOfferResult().code();
    }
};
}
