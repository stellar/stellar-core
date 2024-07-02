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
#include "util/ProtocolVersion.h"

namespace stellar
{

static Price
getInversePrice(Price const& price)
{
    return Price{price.d, price.n};
}

ManageBuyOfferOpFrame::ManageBuyOfferOpFrame(Operation const& op,
                                             TransactionFrame const& parentTx)
    : ManageOfferOpFrameBase(
          op, parentTx, op.body.manageBuyOfferOp().selling,
          op.body.manageBuyOfferOp().buying, op.body.manageBuyOfferOp().offerID,
          getInversePrice(op.body.manageBuyOfferOp().price), false)
    , mManageBuyOffer(mOperation.body.manageBuyOfferOp())
{
}

bool
ManageBuyOfferOpFrame::isOpSupported(LedgerHeader const& header) const
{
    return protocolVersionStartsFrom(header.ledgerVersion,
                                     ProtocolVersion::V_11);
}

bool
ManageBuyOfferOpFrame::isAmountValid() const
{
    return mManageBuyOffer.buyAmount >= 0;
}

bool
ManageBuyOfferOpFrame::isDeleteOffer() const
{
    return mManageBuyOffer.buyAmount == 0;
}

int64_t
ManageBuyOfferOpFrame::getOfferBuyingLiabilities() const
{
    auto res = exchangeV10WithoutPriceErrorThresholds(
        getInversePrice(mManageBuyOffer.price), INT64_MAX, INT64_MAX, INT64_MAX,
        mManageBuyOffer.buyAmount, RoundingType::NORMAL);
    return res.numSheepSend;
}

int64_t
ManageBuyOfferOpFrame::getOfferSellingLiabilities() const
{
    auto res = exchangeV10WithoutPriceErrorThresholds(
        getInversePrice(mManageBuyOffer.price), INT64_MAX, INT64_MAX, INT64_MAX,
        mManageBuyOffer.buyAmount, RoundingType::NORMAL);
    return res.numWheatReceived;
}

void
ManageBuyOfferOpFrame::applyOperationSpecificLimits(int64_t& maxSheepSend,
                                                    int64_t sheepSent,
                                                    int64_t& maxWheatReceive,
                                                    int64_t wheatReceived) const
{
    maxWheatReceive =
        std::min(mManageBuyOffer.buyAmount - wheatReceived, maxWheatReceive);
}

void
ManageBuyOfferOpFrame::getExchangeParametersBeforeV10(
    int64_t& maxSheepSend, int64_t& maxWheatReceive) const
{
    throw std::runtime_error("ManageBuyOffer used before protocol version 10");
}

ManageOfferSuccessResult&
ManageBuyOfferOpFrame::getSuccessResult(OperationResult& res) const
{
    return res.tr().manageBuyOfferResult().success();
}

void
ManageBuyOfferOpFrame::setResultSuccess(OperationResult& res) const
{
    res.tr().manageBuyOfferResult().code(MANAGE_BUY_OFFER_SUCCESS);
}

void
ManageBuyOfferOpFrame::setResultMalformed(OperationResult& res) const
{
    res.tr().manageBuyOfferResult().code(MANAGE_BUY_OFFER_MALFORMED);
}

void
ManageBuyOfferOpFrame::setResultSellNoTrust(OperationResult& res) const
{
    res.tr().manageBuyOfferResult().code(MANAGE_BUY_OFFER_SELL_NO_TRUST);
}

void
ManageBuyOfferOpFrame::setResultBuyNoTrust(OperationResult& res) const
{
    res.tr().manageBuyOfferResult().code(MANAGE_BUY_OFFER_BUY_NO_TRUST);
}

void
ManageBuyOfferOpFrame::setResultSellNotAuthorized(OperationResult& res) const
{
    res.tr().manageBuyOfferResult().code(MANAGE_BUY_OFFER_SELL_NOT_AUTHORIZED);
}

void
ManageBuyOfferOpFrame::setResultBuyNotAuthorized(OperationResult& res) const
{
    res.tr().manageBuyOfferResult().code(MANAGE_BUY_OFFER_BUY_NOT_AUTHORIZED);
}

void
ManageBuyOfferOpFrame::setResultLineFull(OperationResult& res) const
{
    res.tr().manageBuyOfferResult().code(MANAGE_BUY_OFFER_LINE_FULL);
}

void
ManageBuyOfferOpFrame::setResultUnderfunded(OperationResult& res) const
{
    res.tr().manageBuyOfferResult().code(MANAGE_BUY_OFFER_UNDERFUNDED);
}

void
ManageBuyOfferOpFrame::setResultCrossSelf(OperationResult& res) const
{
    res.tr().manageBuyOfferResult().code(MANAGE_BUY_OFFER_CROSS_SELF);
}

void
ManageBuyOfferOpFrame::setResultSellNoIssuer(OperationResult& res) const
{
    res.tr().manageBuyOfferResult().code(MANAGE_BUY_OFFER_SELL_NO_ISSUER);
}

void
ManageBuyOfferOpFrame::setResultBuyNoIssuer(OperationResult& res) const
{
    res.tr().manageBuyOfferResult().code(MANAGE_BUY_OFFER_BUY_NO_ISSUER);
}

void
ManageBuyOfferOpFrame::setResultNotFound(OperationResult& res) const
{
    res.tr().manageBuyOfferResult().code(MANAGE_BUY_OFFER_NOT_FOUND);
}

void
ManageBuyOfferOpFrame::setResultLowReserve(OperationResult& res) const
{
    res.tr().manageBuyOfferResult().code(MANAGE_BUY_OFFER_LOW_RESERVE);
}
}
