#include "transactions/InflationOpFrame.h"


namespace stellar
{
    InflationOpFrame::InflationOpFrame(Operation const& op, OperationResult &res,
        TransactionFrame &parentTx) :
        OperationFrame(op, res, parentTx)
    {

    }
    bool InflationOpFrame::doApply(LedgerDelta& delta, LedgerMaster& ledgerMaster)
    {
        // TODO.2
        innerResult().code(Inflation::NOT_TIME);
        return false;
    }

    bool InflationOpFrame::doCheckValid(Application& app)
    {
        return true;
    }

}
