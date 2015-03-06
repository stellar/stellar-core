#pragma once

#include "transactions/TransactionFrame.h"

namespace stellar
{
    class InflationFrame : public OperationFrame
    {
        Inflation::InflationResult &innerResult() { return mResult.tr().inflationResult(); }
    public:
        InflationFrame(Operation const& op, OperationResult &res, TransactionFrame &parentTx);

        bool doApply(LedgerDelta& delta, LedgerMaster& ledgerMaster);
        bool doCheckValid(Application& app);
    };

    namespace Inflation
    {
        inline Inflation::InflationResultCode getInnerCode(OperationResult const & res)
        {
            return res.tr().inflationResult().code();
        }
    }

}