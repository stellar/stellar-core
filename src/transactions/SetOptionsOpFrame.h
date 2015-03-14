#pragma once

#include "transactions/OperationFrame.h"

namespace stellar
{
class SetOptionsOpFrame : public OperationFrame
{
    int32_t getNeededThreshold();
    SetOptions::SetOptionsResult&
    innerResult()
    {
        return mResult.tr().setOptionsResult();
    }
    SetOptionsOp const& mSetOptions;

  public:
    SetOptionsOpFrame(Operation const& op, OperationResult& res,
                      TransactionFrame& parentTx);

    bool doApply(LedgerDelta& delta, LedgerMaster& ledgerMaster);
    bool doCheckValid(Application& app);
};

namespace SetOptions
{
inline SetOptions::SetOptionsResultCode
getInnerCode(OperationResult const& res)
{
    return res.tr().setOptionsResult().code();
}
}
}