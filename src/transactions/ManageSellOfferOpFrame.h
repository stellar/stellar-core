#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/ManageOfferOpFrameBase.h"

namespace stellar
{

class AbstractLedgerTxn;

class ManageSellOfferOpFrame : public ManageOfferOpFrameBase
{
    ManageSellOfferOp const& mManageSellOffer;

    ManageSellOfferResult&
    innerResult(OperationResult& res) const
    {
        return res.tr().manageSellOfferResult();
    }

  public:
    ManageSellOfferOpFrame(Operation const& op,
                           TransactionFrame const& parentTx);
    ManageSellOfferOpFrame(Operation const& op,
                           TransactionFrame const& parentTx, bool passive);

    bool isAmountValid() const override;
    bool isDeleteOffer() const override;

    int64_t getOfferBuyingLiabilities() const override;
    int64_t getOfferSellingLiabilities() const override;

    void applyOperationSpecificLimits(int64_t& maxSheepSend, int64_t sheepSent,
                                      int64_t& maxWheatReceive,
                                      int64_t wheatReceived) const override;
    void
    getExchangeParametersBeforeV10(int64_t& maxSheepSend,
                                   int64_t& maxWheatReceive) const override;

    ManageOfferSuccessResult&
    getSuccessResult(OperationResult& res) const override;

    void setResultSuccess(OperationResult& res) const override;
    void setResultMalformed(OperationResult& res) const override;
    void setResultSellNoTrust(OperationResult& res) const override;
    void setResultBuyNoTrust(OperationResult& res) const override;
    void setResultSellNotAuthorized(OperationResult& res) const override;
    void setResultBuyNotAuthorized(OperationResult& res) const override;
    void setResultLineFull(OperationResult& res) const override;
    void setResultUnderfunded(OperationResult& res) const override;
    void setResultCrossSelf(OperationResult& res) const override;
    void setResultSellNoIssuer(OperationResult& res) const override;
    void setResultBuyNoIssuer(OperationResult& res) const override;
    void setResultNotFound(OperationResult& res) const override;
    void setResultLowReserve(OperationResult& res) const override;

    static ManageSellOfferResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().manageSellOfferResult().code();
    }
};
}
