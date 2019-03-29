// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "transactions/ManageOfferOpFrame.h"
#include "OfferExchange.h"
#include "database/Database.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "ledger/TrustLineWrapper.h"
#include "main/Application.h"
#include "transactions/TransactionUtils.h"
#include "util/Logging.h"
#include "util/XDROperators.h"
#include "util/types.h"

// convert from sheep to wheat
// selling sheep
// buying wheat

namespace stellar
{

using namespace std;

ManageOfferOpFrame::ManageOfferOpFrame(Operation const& op,
                                       OperationResult& res,
                                       TransactionFrame& parentTx)
    : ManageOfferOpFrame(op, res, parentTx, false)
{
}

ManageOfferOpFrame::ManageOfferOpFrame(Operation const& op,
                                       OperationResult& res,
                                       TransactionFrame& parentTx, bool passive)
    : BaseManageOfferOpFrame(op, res, parentTx,
                             op.body.manageSellOfferOp().selling,
                             op.body.manageSellOfferOp().buying,
                             op.body.manageSellOfferOp().offerID,
                             op.body.manageSellOfferOp().price, passive)
    , mManageSellOffer(mOperation.body.manageSellOfferOp())
{
}

bool
ManageOfferOpFrame::isAmountValid()
{
    return mManageSellOffer.amount >= 0;
}

bool
ManageOfferOpFrame::isDeleteOffer()
{
    return mManageSellOffer.amount == 0;
}

int64_t
ManageOfferOpFrame::getOfferBuyingLiabilities()
{
    auto res = exchangeV10WithoutPriceErrorThresholds(
        mManageSellOffer.price, mManageSellOffer.amount, INT64_MAX, INT64_MAX,
        INT64_MAX, false);
    return res.numSheepSend;
}

int64_t
ManageOfferOpFrame::getOfferSellingLiabilities()
{
    auto res = exchangeV10WithoutPriceErrorThresholds(
        mManageSellOffer.price, mManageSellOffer.amount, INT64_MAX, INT64_MAX,
        INT64_MAX, false);
    return res.numWheatReceived;
}

void
ManageOfferOpFrame::applyOperationSpecificLimits(int64_t& maxSheepSend,
                                                 int64_t sheepSent,
                                                 int64_t& maxWheatReceive,
                                                 int64_t wheatReceived)
{
    maxSheepSend = std::min(mManageSellOffer.amount - sheepSent, maxSheepSend);
}

void
ManageOfferOpFrame::getExchangeParametersBeforeV10(int64_t& maxSheepSend,
                                                   int64_t& maxWheatReceive)
{
    int64_t maxSheepBasedOnWheat;
    if (!bigDivide(maxSheepBasedOnWheat, maxWheatReceive,
                   mManageSellOffer.price.d, mManageSellOffer.price.n,
                   ROUND_DOWN))
    {
        maxSheepBasedOnWheat = INT64_MAX;
    }

    maxSheepSend =
        std::min({maxSheepSend, maxSheepBasedOnWheat, mManageSellOffer.amount});
}

bool
ManageOfferOpFrame::isResultSuccess()
{
    return mResult.tr().manageSellOfferResult().code() ==
           MANAGE_SELL_OFFER_SUCCESS;
}

ManageOfferSuccessResult&
ManageOfferOpFrame::getSuccessResult()
{
    return mResult.tr().manageSellOfferResult().success();
}

void
ManageOfferOpFrame::setResultSuccess()
{
    mResult.tr().manageSellOfferResult().code(MANAGE_SELL_OFFER_SUCCESS);
}

void
ManageOfferOpFrame::setResultMalformed()
{
    mResult.tr().manageSellOfferResult().code(MANAGE_SELL_OFFER_MALFORMED);
}

void
ManageOfferOpFrame::setResultSellNoTrust()
{
    mResult.tr().manageSellOfferResult().code(MANAGE_SELL_OFFER_SELL_NO_TRUST);
}

void
ManageOfferOpFrame::setResultBuyNoTrust()
{
    mResult.tr().manageSellOfferResult().code(MANAGE_SELL_OFFER_BUY_NO_TRUST);
}

void
ManageOfferOpFrame::setResultSellNotAuthorized()
{
    mResult.tr().manageSellOfferResult().code(
        MANAGE_SELL_OFFER_SELL_NOT_AUTHORIZED);
}

void
ManageOfferOpFrame::setResultBuyNotAuthorized()
{
    mResult.tr().manageSellOfferResult().code(
        MANAGE_SELL_OFFER_BUY_NOT_AUTHORIZED);
}

void
ManageOfferOpFrame::setResultLineFull()
{
    mResult.tr().manageSellOfferResult().code(MANAGE_SELL_OFFER_LINE_FULL);
}

void
ManageOfferOpFrame::setResultUnderfunded()
{
    mResult.tr().manageSellOfferResult().code(MANAGE_SELL_OFFER_UNDERFUNDED);
}

void
ManageOfferOpFrame::setResultCrossSelf()
{
    mResult.tr().manageSellOfferResult().code(MANAGE_SELL_OFFER_CROSS_SELF);
}

void
ManageOfferOpFrame::setResultSellNoIssuer()
{
    mResult.tr().manageSellOfferResult().code(MANAGE_SELL_OFFER_SELL_NO_ISSUER);
}

void
ManageOfferOpFrame::setResultBuyNoIssuer()
{
    mResult.tr().manageSellOfferResult().code(MANAGE_SELL_OFFER_BUY_NO_ISSUER);
}

void
ManageOfferOpFrame::setResultNotFound()
{
    mResult.tr().manageSellOfferResult().code(MANAGE_SELL_OFFER_NOT_FOUND);
}

void
ManageOfferOpFrame::setResultLowReserve()
{
    mResult.tr().manageSellOfferResult().code(MANAGE_SELL_OFFER_LOW_RESERVE);
}
}
