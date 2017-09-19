#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/OperationFrame.h"

namespace stellar
{

class PaymentOpFrame : public OperationFrame
{
    PaymentOp const& mPayment;

  public:
    PaymentOpFrame(Operation const& op, TransactionFrame& parentTx);

    OperationResult doApply(Application& app, LedgerDelta& delta,
                 LedgerManager& ledgerManager) override;
    OperationResult doCheckValid(Application& app) override;

    static PaymentResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().paymentResult().code();
    }
};
}
