#pragma once

#include "transactions/TransactionFrame.h"

namespace stellar
{
    class InflationFrame : public TransactionFrame
    {
        Inflation::InflationResult &innerResult() { return mResult.body.tr().inflationResult(); }
    public:
        InflationFrame(const TransactionEnvelope& envelope);

        bool doApply(LedgerDelta& delta, LedgerMaster& ledgerMaster);
        bool doCheckValid(Application& app);
    };

    namespace Inflation
    {
        inline Inflation::InflationResultCode getInnerCode(TransactionResult const & res)
        {
            return res.body.tr().inflationResult().code();
        }
    }

}