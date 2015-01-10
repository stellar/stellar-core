#include "transactions/TransactionFrame.h"

namespace stellar
{
    class MergeFrame : public TransactionFrame
    {

    public:
        MergeFrame(const TransactionEnvelope& envelope);

        bool doApply(TxDelta& delta, LedgerMaster& ledgerMaster);
        bool doCheckValid(Application& app);
    };
}