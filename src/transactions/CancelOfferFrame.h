#include "transactions/TransactionFrame.h"

namespace stellar
{
    class CancelOfferFrame : public TransactionFrame
    {
        
    public:
        CancelOfferFrame(const TransactionEnvelope& envelope);

        bool doApply(LedgerDelta& delta, LedgerMaster& ledgerMaster);
        bool doCheckValid(Application& app);
    };
}