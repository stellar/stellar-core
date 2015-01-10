#include "transactions/InflationFrame.h"


namespace stellar
{
    InflationFrame::InflationFrame(const TransactionEnvelope& envelope) : TransactionFrame(envelope)
    {

    }
    bool InflationFrame::doApply(TxDelta& delta, LedgerMaster& ledgerMaster)
    {
        // TODO.2
        return false;
    }

    bool InflationFrame::doCheckValid(Application& app)
    {
        return true;
    }

}
