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
    : BaseManageOfferOpFrame(op, res, parentTx, op.body.manageOfferOp().selling,
                             op.body.manageOfferOp().buying,
                             op.body.manageOfferOp().offerID,
                             op.body.manageOfferOp().price, passive)
    , mManageOffer(mOperation.body.manageOfferOp())
{
}

bool
ManageOfferOpFrame::isDeleteOffer()
{
    return mManageOffer.amount == 0;
}

int64_t
ManageOfferOpFrame::getOfferBuyingLiabilities()
{
    auto res = exchangeV10WithoutPriceErrorThresholds(
        mManageOffer.price, mManageOffer.amount, INT64_MAX, INT64_MAX,
        INT64_MAX, false);
    return res.numSheepSend;
}

int64_t
ManageOfferOpFrame::getOfferSellingLiabilities()
{
    auto res = exchangeV10WithoutPriceErrorThresholds(
        mManageOffer.price, mManageOffer.amount, INT64_MAX, INT64_MAX,
        INT64_MAX, false);
    return res.numWheatReceived;
}

void
ManageOfferOpFrame::applyOperationSpecificLimits(int64_t& maxSheepSend,
                                                 int64_t sheepSent,
                                                 int64_t& maxWheatReceive,
                                                 int64_t wheatReceived)
{
    maxSheepSend = std::min(mManageOffer.amount - sheepSent, maxSheepSend);
}

void
ManageOfferOpFrame::getExchangeParametersBeforeV10(int64_t& maxSheepSend,
                                                   int64_t& maxWheatReceive)
{
    int64_t maxSheepBasedOnWheat;
    if (!bigDivide(maxSheepBasedOnWheat, maxWheatReceive, mManageOffer.price.d,
                   mManageOffer.price.n, ROUND_DOWN))
    {
        maxSheepBasedOnWheat = INT64_MAX;
    }

    maxSheepSend =
        std::min({maxSheepSend, maxSheepBasedOnWheat, mManageOffer.amount});
}

bool
ManageOfferOpFrame::isResultSuccess()
{
    return mResult.tr().manageOfferResult().code() == MANAGE_OFFER_SUCCESS;
}

ManageOfferSuccessResult&
ManageOfferOpFrame::getSuccessResult()
{
    return mResult.tr().manageOfferResult().success();
}

void
ManageOfferOpFrame::setResultSuccess()
{
    mResult.tr().manageOfferResult().code(MANAGE_OFFER_SUCCESS);
}

void
ManageOfferOpFrame::setResultSellNoTrust()
{
    mResult.tr().manageOfferResult().code(MANAGE_OFFER_SELL_NO_TRUST);
}

void
ManageOfferOpFrame::setResultBuyNoTrust()
{
    mResult.tr().manageOfferResult().code(MANAGE_OFFER_BUY_NO_TRUST);
}

void
ManageOfferOpFrame::setResultSellNotAuthorized()
{
    mResult.tr().manageOfferResult().code(MANAGE_OFFER_SELL_NOT_AUTHORIZED);
}

void
ManageOfferOpFrame::setResultBuyNotAuthorized()
{
    mResult.tr().manageOfferResult().code(MANAGE_OFFER_BUY_NOT_AUTHORIZED);
}

void
ManageOfferOpFrame::setResultLineFull()
{
    mResult.tr().manageOfferResult().code(MANAGE_OFFER_LINE_FULL);
}

void
ManageOfferOpFrame::setResultUnderfunded()
{
    mResult.tr().manageOfferResult().code(MANAGE_OFFER_UNDERFUNDED);
}

void
ManageOfferOpFrame::setResultCrossSelf()
{
    mResult.tr().manageOfferResult().code(MANAGE_OFFER_CROSS_SELF);
}

void
ManageOfferOpFrame::setResultSellNoIssuer()
{
    mResult.tr().manageOfferResult().code(MANAGE_OFFER_SELL_NO_ISSUER);
}

void
ManageOfferOpFrame::setResultBuyNoIssuer()
{
    mResult.tr().manageOfferResult().code(MANAGE_OFFER_BUY_NO_ISSUER);
}

void
ManageOfferOpFrame::setResultNotFound()
{
    mResult.tr().manageOfferResult().code(MANAGE_OFFER_NOT_FOUND);
}

void
ManageOfferOpFrame::setResultLowReserve()
{
    mResult.tr().manageOfferResult().code(MANAGE_OFFER_LOW_RESERVE);
}

// makes sure the currencies are different
bool
ManageOfferOpFrame::doCheckValid(uint32_t ledgerVersion)
{
    Asset const& sheep = mManageOffer.selling;
    Asset const& wheat = mManageOffer.buying;

    if (!isAssetValid(sheep) || !isAssetValid(wheat))
    {
        mResult.tr().manageOfferResult().code(MANAGE_OFFER_MALFORMED);
        return false;
    }
    if (compareAsset(sheep, wheat))
    {
        mResult.tr().manageOfferResult().code(MANAGE_OFFER_MALFORMED);
        return false;
    }
    if (mManageOffer.amount < 0 || mManageOffer.price.d <= 0 ||
        mManageOffer.price.n <= 0)
    {
        mResult.tr().manageOfferResult().code(MANAGE_OFFER_MALFORMED);
        return false;
    }
    if (ledgerVersion > 2 && mManageOffer.offerID == 0 &&
        mManageOffer.amount == 0)
    { // since version 3 of ledger you cannot send
        // offer operation with id and
        // amount both equal to 0
        setResultNotFound();
        return false;
    }

    return true;
}
}
