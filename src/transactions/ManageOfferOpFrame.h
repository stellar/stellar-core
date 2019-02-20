#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/OperationFrame.h"

namespace stellar
{

class AbstractLedgerTxn;

class ManageOfferOpFrame : public OperationFrame
{
    ManageOfferOp const& mManageOffer;

    bool checkOfferValid(AbstractLedgerTxn& lsOuter);

    bool computeOfferExchangeParameters(AbstractLedgerTxn& ltxOuter,
                                        bool creatingNewOffer,
                                        int64_t& maxSheepSend,
                                        int64_t& maxWheatReceive);

    ManageOfferResult&
    innerResult()
    {
        return mResult.tr().manageOfferResult();
    }

    LedgerEntry buildOffer(int64_t amount, uint32_t flags) const;

  protected:
    bool mPassive;

  public:
    ManageOfferOpFrame(Operation const& op, OperationResult& res,
                       TransactionFrame& parentTx);

    bool doApply(AbstractLedgerTxn& lsOuter) override;
    bool doCheckValid(uint32_t ledgerVersion) override;

    static ManageOfferResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().manageOfferResult().code();
    }
};
}
