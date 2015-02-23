#pragma once

#include "transactions/TransactionFrame.h"
#include "ledger/OfferFrame.h"

namespace stellar
{

class PaymentFrame : public TransactionFrame
{
    // destination must exist
    bool sendNoCreate(AccountFrame& destination, LedgerDelta& delta, LedgerMaster& ledgerMaster);

    Payment::PaymentResult &innerResult() { return mResult.body.tr().paymentResult(); }
public:
    PaymentFrame(const TransactionEnvelope& envelope);

    bool doApply(LedgerDelta& delta, LedgerMaster& ledgerMaster);
    bool doCheckValid(Application& app);

};

namespace Payment
{
    inline Payment::PaymentResultCode getInnerCode(TransactionResult const & res)
    {
        return res.body.tr().paymentResult().code();
    }
}

}
