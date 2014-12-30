#include "transactions/TransactionFrame.h"

namespace stellar
{

class PaymentFrame : public TransactionFrame
{
    bool crossOffer(OfferFrame& sellingWheatOffer, int64_t maxWheatReceived, 
        int64_t& numWheatReceived,  int64_t& numSheepReceived, 
        int64_t wheatTransferRate,
        TxDelta& delta, LedgerMaster& ledgerMaster);

    // returns false if tx should be canceled
    bool convert(Currency& sell,
        Currency& buy, int64_t amountToBuy,
        int64_t& retAmountToSell,
        TxDelta& delta, LedgerMaster& ledgerMaster);
    bool sendCredit(AccountFrame& receiver, TxDelta& delta, LedgerMaster& ledgerMaster);
public:
    void doApply(TxDelta& delta, LedgerMaster& ledgerMaster);
    bool doCheckValid(Application& app);
};


}
