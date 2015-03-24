#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "transactions/OperationFrame.h"
#include "ledger/TrustFrame.h"
#include "ledger/OfferFrame.h"

namespace stellar
{
class CreateOfferOpFrame : public OperationFrame
{
    TrustFrame mSheepLineA;
    TrustFrame mWheatLineA;

    OfferFrame mSellSheepOffer;

    bool checkOfferValid(Database& db);

    CreateOfferResult&
    innerResult()
    {
        return mResult.tr().createOfferResult();
    }

    CreateOfferOp const& mCreateOffer;

  public:
    CreateOfferOpFrame(Operation const& op, OperationResult& res,
                       TransactionFrame& parentTx);

    bool doApply(LedgerDelta& delta, LedgerManager& ledgerManager);
    bool doCheckValid(Application& app);

    static CreateOfferResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().createOfferResult().code();
    }
};
}
