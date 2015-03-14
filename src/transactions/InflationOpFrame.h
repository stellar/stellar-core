#pragma once

#include "transactions/OperationFrame.h"

namespace stellar
{
class InflationOpFrame : public OperationFrame
{
    Inflation::InflationResult&
    innerResult()
    {
        return mResult.tr().inflationResult();
    }

  public:
    InflationOpFrame(Operation const& op, OperationResult& res,
                     TransactionFrame& parentTx);

    bool doApply(LedgerDelta& delta, LedgerMaster& ledgerMaster);
    bool doCheckValid(Application& app);
};

namespace Inflation
{
inline Inflation::InflationResultCode
getInnerCode(OperationResult const& res)
{
    return res.tr().inflationResult().code();
}
}
}