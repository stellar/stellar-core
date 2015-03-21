#pragma once

#include "transactions/OperationFrame.h"

namespace stellar
{
class AllowTrustOpFrame : public OperationFrame
{
    int32_t getNeededThreshold() const;
    AllowTrust::AllowTrustResult&
    innerResult() const
    {
        return getResult().tr().allowTrustResult();
    }

    AllowTrustOp const& mAllowTrust;

  public:
    AllowTrustOpFrame(Operation const& op, OperationResult& res,
                      TransactionFrame& parentTx);

    bool doApply(LedgerDelta& delta, LedgerManagerImpl& ledgerMaster);
    bool doCheckValid(Application& app);
};

namespace AllowTrust
{
inline AllowTrust::AllowTrustResultCode
getInnerCode(OperationResult const& res)
{
    return res.tr().allowTrustResult().code();
}
}
}
