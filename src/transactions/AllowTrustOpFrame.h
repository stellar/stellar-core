#pragma once

#include "transactions/OperationFrame.h"

namespace stellar
{
class AllowTrustOpFrame : public OperationFrame
{
    int32_t getNeededThreshold();
    AllowTrust::AllowTrustResult&
    innerResult()
    {
        return getResult().tr().allowTrustResult();
    }

    AllowTrustOp const& mAllowTrust;

  public:
    AllowTrustOpFrame(Operation const& op, OperationResult& res,
                      TransactionFrame& parentTx);

    bool doApply(LedgerDelta& delta, LedgerMaster& ledgerMaster);
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
