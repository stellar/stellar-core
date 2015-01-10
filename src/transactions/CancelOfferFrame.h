#include "transactions/TransactionFrame.h"

namespace stellar
{
    class CancelOfferFrame : public TransactionFrame
    {
        
    public:
        CancelOfferFrame(const TransactionEnvelope& envelope);

        bool doApply(TxDelta& delta, LedgerMaster& ledgerMaster);
        bool doCheckValid(Application& app);
    };
}