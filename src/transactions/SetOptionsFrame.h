#pragma once

#include "transactions/TransactionFrame.h"

namespace stellar
{
    class SetOptionsFrame : public TransactionFrame
    {
        int32_t getNeededThreshold();
        SetOptions::SetOptionsResult &innerResult() { return mResult.body.tr().setOptionsResult(); }
    public:
        SetOptionsFrame(const TransactionEnvelope& envelope);

        bool doApply(LedgerDelta& delta, LedgerMaster& ledgerMaster);
        bool doCheckValid(Application& app);
    };

    namespace SetOptions
    {
        inline SetOptions::SetOptionsResultCode getInnerCode(TransactionResult const & res)
        {
            return res.body.tr().setOptionsResult().result.code();
        }
    }

}