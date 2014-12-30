#include "transactions/TransactionFrame.h"

namespace stellar
{
    class InflationFrame : public TransactionFrame
    {

    public:
        void doApply(TxDelta& delta, LedgerMaster& ledgerMaster);
        bool doCheckValid(Application& app);
    };
}