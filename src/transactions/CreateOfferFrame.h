#include "transactions/TransactionFrame.h"

namespace stellar
{
    class CreateOfferFrame : public TransactionFrame
    {
        bool checkCross(TxDelta& delta, LedgerMaster& ledgerMaster);
    public:
        void doApply(TxDelta& delta, LedgerMaster& ledgerMaster);
        bool doCheckValid(LedgerMaster& ledgerMaster);
    };
}