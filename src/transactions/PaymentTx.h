#include "transactions/Transaction.h"

namespace stellar
{
    class PaymentTx : public Transaction
    {
        int64_t convert(int64_t amountToFill, stellarxdr::CurrencyIssuer& source,
            stellarxdr::CurrencyIssuer& dest, TxDelta& delta, LedgerMaster& ledgerMaster);
        void sendCredit(AccountEntry& receiver, TxDelta& delta, LedgerMaster& ledgerMaster);
    public:
        void doApply(TxDelta& delta, LedgerMaster& ledgerMaster);
        bool doCheckValid(LedgerMaster& ledgerMaster);
    };
}
