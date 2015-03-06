#include "transactions/InflationFrame.h"


namespace stellar
{
    InflationFrame::InflationFrame(Operation const& op, OperationResult &res,
        TransactionFrame &parentTx) :
        OperationFrame(op, res, parentTx)
    {

    }
    bool InflationFrame::doApply(LedgerDelta& delta, LedgerMaster& ledgerMaster)
    {
        // TODO.2
        innerResult().code(Inflation::NOT_TIME);
        return false;
    }

    bool InflationFrame::doCheckValid(Application& app)
    {
        return true;
    }

}
