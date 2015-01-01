#include "transactions/TransactionFrame.h"

namespace stellar
{
    class AllowTrustTxFrame : public TransactionFrame
    {
    public:
        AllowTrustTxFrame(const TransactionEnvelope& envelope);

        void doApply(TxDelta& delta, LedgerMaster& ledgerMaster);
        bool doCheckValid(Application& app);
    };
}
