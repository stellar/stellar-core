// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/ManageBuyOfferOpFrame.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "ledger/TrustLineWrapper.h"
#include "transactions/OfferExchange.h"
#include "transactions/TransactionUtils.h"

namespace stellar
{

ManageBuyOfferOpFrame::ManageBuyOfferOpFrame(Operation const& op,
                                             OperationResult& res,
                                             TransactionFrame& parentTx)
    : BaseManageOfferOpFrame(
          op, res, parentTx, op.body.manageBuyOfferOp().selling,
          op.body.manageBuyOfferOp().buying, op.body.manageBuyOfferOp().offerID,
          op.body.manageBuyOfferOp().price, false)
    , mManageBuyOffer(mOperation.body.manageBuyOfferOp())
{
}

bool
ManageBuyOfferOpFrame::isVersionSupported(uint32_t protocolVersion) const
{
    return protocolVersion >= 11;
}

bool
ManageBuyOfferOpFrame::isDeleteOffer()
{
    return mManageBuyOffer.buyAmount == 0;
}

int64_t
ManageBuyOfferOpFrame::getOfferBuyingLiabilities()
{
    auto res = exchangeV10WithoutPriceErrorThresholds(
        mManageBuyOffer.price, INT64_MAX, INT64_MAX, INT64_MAX,
        mManageBuyOffer.buyAmount, false);
    return res.numSheepSend;
}

int64_t
ManageBuyOfferOpFrame::getOfferSellingLiabilities()
{
    auto res = exchangeV10WithoutPriceErrorThresholds(
        mManageBuyOffer.price, INT64_MAX, INT64_MAX, INT64_MAX,
        mManageBuyOffer.buyAmount, false);
    return res.numWheatReceived;
}

void
ManageBuyOfferOpFrame::applyOperationSpecificLimits(int64_t& maxSheepSend,
                                                    int64_t sheepSent,
                                                    int64_t& maxWheatReceive,
                                                    int64_t wheatReceived)
{
    maxWheatReceive =
        std::min(mManageBuyOffer.buyAmount - wheatReceived, maxWheatReceive);
}

void
ManageBuyOfferOpFrame::getExchangeParametersBeforeV10(int64_t& maxSheepSend,
                                                      int64_t& maxWheatReceive)
{
    throw std::runtime_error("ManageBuyOffer used before protocol version 10");
}

bool
ManageBuyOfferOpFrame::isResultSuccess()
{
    return mResult.tr().manageBuyOfferResult().code() ==
           MANAGE_BUY_OFFER_SUCCESS;
}

ManageOfferSuccessResult&
ManageBuyOfferOpFrame::getSuccessResult()
{
    return mResult.tr().manageBuyOfferResult().success();
}

void
ManageBuyOfferOpFrame::setResultSuccess()
{
    mResult.tr().manageBuyOfferResult().code(MANAGE_BUY_OFFER_SUCCESS);
}

void
ManageBuyOfferOpFrame::setResultSellNoTrust()
{
    mResult.tr().manageBuyOfferResult().code(MANAGE_BUY_OFFER_SELL_NO_TRUST);
}

void
ManageBuyOfferOpFrame::setResultBuyNoTrust()
{
    mResult.tr().manageBuyOfferResult().code(MANAGE_BUY_OFFER_BUY_NO_TRUST);
}

void
ManageBuyOfferOpFrame::setResultSellNotAuthorized()
{
    mResult.tr().manageBuyOfferResult().code(
        MANAGE_BUY_OFFER_SELL_NOT_AUTHORIZED);
}

void
ManageBuyOfferOpFrame::setResultBuyNotAuthorized()
{
    mResult.tr().manageBuyOfferResult().code(
        MANAGE_BUY_OFFER_BUY_NOT_AUTHORIZED);
}

void
ManageBuyOfferOpFrame::setResultLineFull()
{
    mResult.tr().manageBuyOfferResult().code(MANAGE_BUY_OFFER_LINE_FULL);
}

void
ManageBuyOfferOpFrame::setResultUnderfunded()
{
    mResult.tr().manageBuyOfferResult().code(MANAGE_BUY_OFFER_UNDERFUNDED);
}

void
ManageBuyOfferOpFrame::setResultCrossSelf()
{
    mResult.tr().manageBuyOfferResult().code(MANAGE_BUY_OFFER_CROSS_SELF);
}

void
ManageBuyOfferOpFrame::setResultSellNoIssuer()
{
    mResult.tr().manageBuyOfferResult().code(MANAGE_BUY_OFFER_SELL_NO_ISSUER);
}

void
ManageBuyOfferOpFrame::setResultBuyNoIssuer()
{
    mResult.tr().manageBuyOfferResult().code(MANAGE_BUY_OFFER_BUY_NO_ISSUER);
}

void
ManageBuyOfferOpFrame::setResultNotFound()
{
    mResult.tr().manageBuyOfferResult().code(MANAGE_BUY_OFFER_NOT_FOUND);
}

void
ManageBuyOfferOpFrame::setResultLowReserve()
{
    mResult.tr().manageBuyOfferResult().code(MANAGE_BUY_OFFER_LOW_RESERVE);
}

bool
ManageBuyOfferOpFrame::doCheckValid(uint32_t ledgerVersion)
{
    Asset const& sheep = mManageBuyOffer.selling;
    Asset const& wheat = mManageBuyOffer.buying;

    if (!isAssetValid(sheep) || !isAssetValid(wheat))
    {
        innerResult().code(MANAGE_BUY_OFFER_MALFORMED);
        return false;
    }
    if (compareAsset(sheep, wheat))
    {
        innerResult().code(MANAGE_BUY_OFFER_MALFORMED);
        return false;
    }
    if (mManageBuyOffer.buyAmount < 0 || mManageBuyOffer.price.d <= 0 ||
        mManageBuyOffer.price.n <= 0)
    {
        innerResult().code(MANAGE_BUY_OFFER_MALFORMED);
        return false;
    }
    if (mManageBuyOffer.offerID == 0 && mManageBuyOffer.buyAmount == 0)
    {
        // Note: This currently does not match the return code of ManageOfferOp.
        // This is not an accident, but instead reflects the fact that MALFORMED
        // is a more relevant error for this than NOT_FOUND. MALFORMED correctly
        // reflects that this is never valid, whereas NOT_FOUND suggests that it
        // depends on the state of the ledger. This was not corrected in
        // ManageOfferOp because changing the behavior of ManageOfferOp is not
        // in the scope of CAP-0006.
        innerResult().code(MANAGE_BUY_OFFER_MALFORMED);
        return false;
    }

    return true;
}
}
