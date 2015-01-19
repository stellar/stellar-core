#include "transactions/TransactionFrame.h"
#include "ledger/OfferFrame.h"

namespace stellar
{

class PaymentFrame : public TransactionFrame
{
    bool crossOffer(OfferFrame& sellingWheatOffer, int64_t maxWheatReceived, 
        int64_t& numWheatReceived,  int64_t& numSheepReceived, 
        int64_t wheatTransferRate,
        LedgerDelta& delta, LedgerMaster& ledgerMaster);

    // returns false if tx should be canceled
    bool convert(Currency& sell,
        Currency& buy, int64_t amountToBuy,
        int64_t& retAmountToSell,
        LedgerDelta& delta, LedgerMaster& ledgerMaster);
    bool sendCredit(AccountFrame& receiver, LedgerDelta& delta, LedgerMaster& ledgerMaster);
public:
    PaymentFrame(const TransactionEnvelope& envelope);

    bool doApply(LedgerDelta& delta, LedgerMaster& ledgerMaster);
    bool doCheckValid(Application& app);
};


}
