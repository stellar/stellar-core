#pragma once

// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/ManageOfferOpFrameBase.h"

namespace stellar
{

class AbstractLedgerTxn;

class ManageBuyOfferOpFrame : public ManageOfferOpFrameBase
{
    ManageBuyOfferOp const& mManageBuyOffer;

    bool isOpSupported(LedgerHeader const& header) const override;

    ManageBuyOfferResult&
    innerResult()
    {
        return mResult.tr().manageBuyOfferResult();
    }

  public:
    ManageBuyOfferOpFrame(Operation const& op, OperationResult& res,
                          TransactionFrame& parentTx);

    bool isAmountValid() const override;
    bool isDeleteOffer() const override;

    int64_t getOfferBuyingLiabilities() const override;
    int64_t getOfferSellingLiabilities() const override;

    void applyOperationSpecificLimits(int64_t& maxSheepSend, int64_t sheepSent,
                                      int64_t& maxWheatReceive,
                                      int64_t wheatReceived) override;
    void getExchangeParametersBeforeV10(int64_t& maxSheepSend,
                                        int64_t& maxWheatReceive) override;

    ManageOfferSuccessResult& getSuccessResult() override;

    void setResultSuccess() override;
    void setResultMalformed() override;
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

    static ManageBuyOfferResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().manageBuyOfferResult().code();
    }
};
}
