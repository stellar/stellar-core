#include "transactions/InflationFrame.h"

// TODO.2 still 

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
        // TODO.2
        return false;
    }

}
