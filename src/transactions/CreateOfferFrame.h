#include "transactions/TransactionFrame.h"

namespace stellar
{
    class CreateOfferFrame : public TransactionFrame
    {
        TrustFrame mSheepLineA;
        TrustFrame mWheatLineA;

        int64_t mWheatTransferRate;
        int64_t mSheepTransferRate;

        OfferFrame mSellSheepOffer;

        bool checkOfferValid(LedgerMaster& ledgerMaster); 
        
        bool crossOffer(OfferFrame& sellingWheatOffer,
            int64_t maxSheepReceived, int64_t& amountSheepReceived,
            TxDelta& delta, LedgerMaster& ledgerMaster);

        

        bool convert(Currency& sheep,
            Currency& wheat, int64_t amountOfSheepToSell, int64_t minSheepPrice,
            TxDelta& delta, LedgerMaster& ledgerMaster);

    public:
        CreateOfferFrame(const TransactionEnvelope& envelope);

        bool doApply(TxDelta& delta, LedgerMaster& ledgerMaster);
        bool doCheckValid(Application& app);
    };
}