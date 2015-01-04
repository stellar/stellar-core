#include "transactions/InflationFrame.h"


namespace stellar
{
    InflationFrame::InflationFrame(const TransactionEnvelope& envelope) : TransactionFrame(envelope)
    {

    }
    void InflationFrame::doApply(TxDelta& delta, LedgerMaster& ledgerMaster)
    {
        // TODO.2
    }

    bool InflationFrame::doCheckValid(Application& app)
    {
        return true;
    }

}
