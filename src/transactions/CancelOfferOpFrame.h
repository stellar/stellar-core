#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "transactions/OperationFrame.h"

namespace stellar
{
class CancelOfferOpFrame : public OperationFrame
{
    CancelOfferResult&
    innerResult()
    {
        return mResult.tr().cancelOfferResult();
    }

  public:
    CancelOfferOpFrame(Operation const& op, OperationResult& res,
                       TransactionFrame& parentTx);

    bool doApply(LedgerDelta& delta, LedgerManager& ledgerManager);
    bool doCheckValid(Application& app);

    static CancelOfferResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().cancelOfferResult().code();
    }
};
}
