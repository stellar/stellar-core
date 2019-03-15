#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/BaseManageOfferOpFrame.h"

namespace stellar
{

class AbstractLedgerTxn;

class ManageOfferOpFrame : public BaseManageOfferOpFrame
{
    ManageOfferOp const& mManageOffer;

    ManageOfferResult&
    innerResult()
    {
        return mResult.tr().manageOfferResult();
    }

  public:
    ManageOfferOpFrame(Operation const& op, OperationResult& res,
                       TransactionFrame& parentTx);
    ManageOfferOpFrame(Operation const& op, OperationResult& res,
                       TransactionFrame& parentTx, bool passive);

    bool doCheckValid(uint32_t ledgerVersion) override;

    bool isDeleteOffer() override;

    int64_t getOfferBuyingLiabilities() override;
    int64_t getOfferSellingLiabilities() override;

    void applyOperationSpecificLimits(int64_t& maxSheepSend, int64_t sheepSent,
                                      int64_t& maxWheatReceive,
                                      int64_t wheatReceived) override;
    void getExchangeParametersBeforeV10(int64_t& maxSheepSend,
                                        int64_t& maxWheatReceive) override;

    bool isResultSuccess() override;
    ManageOfferSuccessResult& getSuccessResult() override;

    void setResultSuccess() override;
    void setResultSellNoTrust() override;
    void setResultBuyNoTrust() override;
    void setResultSellNotAuthorized() override;
    void setResultBuyNotAuthorized() override;
    void setResultLineFull() override;
    void setResultUnderfunded() override;
    void setResultCrossSelf() override;
    void setResultSellNoIssuer() override;
    void setResultBuyNoIssuer() override;
    void setResultNotFound() override;
    void setResultLowReserve() override;

    static ManageOfferResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().manageOfferResult().code();
    }
};
}
