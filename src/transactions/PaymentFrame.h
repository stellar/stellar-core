#pragma once

#include "transactions/TransactionFrame.h"
#include "ledger/OfferFrame.h"

namespace stellar
{

class PaymentFrame : public OperationFrame
{
    // destination must exist
    bool sendNoCreate(AccountFrame& destination, LedgerDelta& delta, LedgerMaster& ledgerMaster);

    Payment::PaymentResult &innerResult() { return mResult.tr().paymentResult(); }
    PaymentOp const& mPayment;
public:
    PaymentFrame(Operation const& op, OperationResult &res, TransactionFrame &parentTx);

    bool doApply(LedgerDelta& delta, LedgerMaster& ledgerMaster);
    bool doCheckValid(Application& app);

};

namespace Payment
{
    inline Payment::PaymentResultCode getInnerCode(OperationResult const & res)
    {
        return res.tr().paymentResult().code();
    }
}

}
