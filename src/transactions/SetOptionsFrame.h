#pragma once

#include "transactions/TransactionFrame.h"

namespace stellar
{
    class SetOptionsFrame : public OperationFrame
    {
        int32_t getNeededThreshold();
        SetOptions::SetOptionsResult &innerResult() { return mResult.tr().setOptionsResult(); }
        SetOptionsTx const& mSetOptions;
    public:
        SetOptionsFrame(Operation const& op, OperationResult &res, TransactionFrame &parentTx);

        bool doApply(LedgerDelta& delta, LedgerMaster& ledgerMaster);
        bool doCheckValid(Application& app);
    };

    namespace SetOptions
    {
        inline SetOptions::SetOptionsResultCode getInnerCode(OperationResult const & res)
        {
            return res.tr().setOptionsResult().code();
        }
    }

}