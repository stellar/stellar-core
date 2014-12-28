#include "transactions/TransactionFrame.h"

namespace stellar
{
    class PaymentFrame : public TransactionFrame
    {
        // returns the amount you have to sell to buy this much
        // 0 for not enough offers
        int64_t convert(Currency& sell,
            Currency& buy, int64_t amountToBuy,
            TxDelta& delta, LedgerMaster& ledgerMaster);
        void sendCredit(AccountFrame& receiver, TxDelta& delta, LedgerMaster& ledgerMaster);
    public:
        void doApply(TxDelta& delta, LedgerMaster& ledgerMaster);
        bool doCheckValid(LedgerMaster& ledgerMaster);
    };
}
