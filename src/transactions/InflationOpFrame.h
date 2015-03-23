#pragma once

#include "transactions/OperationFrame.h"

namespace stellar
{
class InflationOpFrame : public OperationFrame
{
    InflationResult&
    innerResult()
    {
        return mResult.tr().inflationResult();
    }

  public:
    InflationOpFrame(Operation const& op, OperationResult& res,
                     TransactionFrame& parentTx);

    bool doApply(LedgerDelta& delta, LedgerManagerImpl& ledgerMaster);
    bool doCheckValid(Application& app);

    static InflationResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().inflationResult().code();
    }
};
}