#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/TrustFrame.h"
#include "transactions/OperationFrame.h"
#include "util/optional.h"

namespace stellar
{
class ManageOfferOpFrame : public OperationFrame
{
    bool checkOfferValid(medida::MetricsRegistry& metrics, LedgerEntries& entries,
                         LedgerDelta& ledgerDelta);

    ManageOfferResult&
    innerResult()
    {
        return mResult.tr().manageOfferResult();
    }

    ManageOfferOp const& mManageOffer;

    OfferEntry makeOffer(AccountID const& account, ManageOfferOp const& op,
                         uint32 flags);

  protected:
    bool mPassive;

  public:
    ManageOfferOpFrame(Operation const& op, OperationResult& res,
                       TransactionFrame& parentTx);

    bool doApply(Application& app, LedgerDelta& ledgerDelta,
                 LedgerManager& ledgerManager) override;
    bool doCheckValid(Application& app) override;

    static ManageOfferResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().manageOfferResult().code();
    }
};
}
