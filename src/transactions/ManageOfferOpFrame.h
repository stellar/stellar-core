#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/OperationFrame.h"

namespace stellar
{
class LedgerState;

class ManageOfferOpFrame : public OperationFrame
{
    bool checkOfferValid(medida::MetricsRegistry& metrics,
                         LedgerState& ls);

    ManageOfferResult&
    innerResult()
    {
        return mResult.tr().manageOfferResult();
    }

    ManageOfferOp const& mManageOffer;

    OfferEntry buildOffer(AccountID const& account, ManageOfferOp const& op,
                          uint32 flags);

  protected:
    bool mPassive;

  public:
    ManageOfferOpFrame(Operation const& op, OperationResult& res,
                       TransactionFrame& parentTx);

    bool doApply(Application& app, LedgerState& lsOuter) override;
    bool doCheckValid(Application& app, uint32_t ledgerVersion) override;

    static ManageOfferResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().manageOfferResult().code();
    }
};
}
