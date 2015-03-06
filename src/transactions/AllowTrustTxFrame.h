#pragma once

#include "transactions/TransactionFrame.h"

namespace stellar
{
    class AllowTrustTxFrame : public OperationFrame
    {
        int32_t getNeededThreshold();
        AllowTrust::AllowTrustResult &innerResult() { return mResult.tr().allowTrustResult(); }

        AllowTrustOp const& mAllowTrust;
    public:
        AllowTrustTxFrame(Operation const& op, OperationResult &res, TransactionFrame &parentTx);

        bool doApply(LedgerDelta& delta, LedgerMaster& ledgerMaster);
        bool doCheckValid(Application& app);
    };

    namespace AllowTrust
    {
        inline AllowTrust::AllowTrustResultCode getInnerCode(OperationResult const & res)
        {
            return res.tr().allowTrustResult().code();
        }
    }
}
