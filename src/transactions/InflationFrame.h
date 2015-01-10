#include "transactions/TransactionFrame.h"

namespace stellar
{
    class InflationFrame : public TransactionFrame
    {

    public:
        InflationFrame(const TransactionEnvelope& envelope);

        bool doApply(TxDelta& delta, LedgerMaster& ledgerMaster);
        bool doCheckValid(Application& app);
    };
}