#pragma once

#include "transactions/TransactionFrame.h"

namespace stellar
{
    class AllowTrustTxFrame : public TransactionFrame
    {
        int32_t getNeededThreshold();
        AllowTrust::AllowTrustResult &innerResult() { return mResult.body.tr().allowTrustResult(); }
    public:
        AllowTrustTxFrame(const TransactionEnvelope& envelope);

        bool doApply(LedgerDelta& delta, LedgerMaster& ledgerMaster);
        bool doCheckValid(Application& app);
    };

    namespace AllowTrust
    {
        inline AllowTrust::AllowTrustResultCode getInnerCode(TransactionResult const & res)
        {
            return res.body.tr().allowTrustResult().code();
        }
    }
}
