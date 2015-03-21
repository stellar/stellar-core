#pragma once

#include "transactions/OperationFrame.h"

namespace stellar
{
class SetOptionsOpFrame : public OperationFrame
{
    int32_t getNeededThreshold() const;
    SetOptions::SetOptionsResult&
    innerResult()
    {
        return mResult.tr().setOptionsResult();
    }
    SetOptionsOp const& mSetOptions;

  public:
    SetOptionsOpFrame(Operation const& op, OperationResult& res,
                      TransactionFrame& parentTx);

    bool doApply(LedgerDelta& delta, LedgerManagerImpl& ledgerMaster);
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
