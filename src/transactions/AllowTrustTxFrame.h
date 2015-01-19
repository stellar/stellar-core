#include "transactions/TransactionFrame.h"

namespace stellar
{
    class AllowTrustTxFrame : public TransactionFrame
    {
        int32_t getNeededThreshold();
    public:
        AllowTrustTxFrame(const TransactionEnvelope& envelope);

        bool doApply(LedgerDelta& delta, LedgerMaster& ledgerMaster);
        bool doCheckValid(Application& app);
    };
}
