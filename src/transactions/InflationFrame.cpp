#include "transactions/InflationFrame.h"


namespace stellar
{
    InflationFrame::InflationFrame(const TransactionEnvelope& envelope) : TransactionFrame(envelope)
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
