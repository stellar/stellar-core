// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/PathPaymentStrictReceiveOpFrame.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "ledger/TrustLineWrapper.h"
#include "transactions/TransactionUtils.h"
#include "util/XDROperators.h"
#include <Tracy.hpp>

namespace stellar
{

PathPaymentStrictReceiveOpFrame::PathPaymentStrictReceiveOpFrame(
    Operation const& op, OperationResult& res, TransactionFrame& parentTx)
    : PathPaymentOpFrameBase(op, res, parentTx)
    , mPathPayment(mOperation.body.pathPaymentStrictReceiveOp())
{
}

bool
PathPaymentStrictReceiveOpFrame::doApply(AbstractLedgerTxn& ltx)
{
    ZoneNamedN(applyZone, "PathPaymentStrictReceiveOp apply", true);
    std::string pathStr = assetToString(getSourceAsset());
    for (auto const& asset : mPathPayment.path)
    {
        pathStr += "->";
        pathStr += assetToString(asset);
    }
    pathStr += "->";
    pathStr += assetToString(getDestAsset());
    ZoneTextV(applyZone, pathStr.c_str(), pathStr.size());

    setResultSuccess();

    bool doesSourceAccountExist = true;
    if (ltx.loadHeader().current().ledgerVersion < 8)
    {
        doesSourceAccountExist =
            (bool)stellar::loadAccountWithoutRecord(ltx, getSourceID());
    }

    bool bypassIssuerCheck = shouldBypassIssuerCheck(mPathPayment.path);
    if (!bypassIssuerCheck)
    {
        if (!stellar::loadAccountWithoutRecord(ltx, getDestID()))
        {
            setResultNoDest();
            return false;
        }
    }

    if (!updateDestBalance(ltx, mPathPayment.destAmount, bypassIssuerCheck))
    {
        return false;
    }
    innerResult().success().last = SimplePaymentResult(
        getDestID(), getDestAsset(), mPathPayment.destAmount);

    // build the full path from the destination, ending with sendAsset
    std::vector<Asset> fullPath;
    fullPath.insert(fullPath.end(), mPathPayment.path.rbegin(),
                    mPathPayment.path.rend());
    fullPath.emplace_back(getSourceAsset());

    // Walk the path
    Asset recvAsset = getDestAsset();
    int64_t maxAmountRecv = mPathPayment.destAmount;
    for (auto const& sendAsset : fullPath)
    {
        if (recvAsset == sendAsset)
        {
            continue;
        }

        if (!checkIssuer(ltx, sendAsset))
        {
            return false;
        }

        int64_t maxOffersToCross = INT64_MAX;
        if (ltx.loadHeader().current().ledgerVersion >=
            FIRST_PROTOCOL_SUPPORTING_OPERATION_LIMITS)
        {
            size_t offersCrossed = innerResult().success().offers.size();
            // offersCrossed will never be bigger than INT64_MAX because
            // - the machine would have run out of memory
            // - the limit, which cannot exceed INT64_MAX, should be enforced
            // so this subtraction is safe because MAX_OFFERS_TO_CROSS >= 0
            maxOffersToCross = MAX_OFFERS_TO_CROSS - offersCrossed;
        }

        int64_t amountSend = 0;
        int64_t amountRecv = 0;
        std::vector<ClaimOfferAtom> offerTrail;
        if (!convert(ltx, maxOffersToCross, sendAsset, INT64_MAX, amountSend,
                     recvAsset, maxAmountRecv, amountRecv,
                     RoundingType::PATH_PAYMENT_STRICT_RECEIVE, offerTrail))
        {
            return false;
        }

        maxAmountRecv = amountSend;
        recvAsset = sendAsset;

        // add offers that got taken on the way
        // insert in front to match the path's order
        auto& offers = innerResult().success().offers;
        offers.insert(offers.begin(), offerTrail.begin(), offerTrail.end());
    }

    if (maxAmountRecv > mPathPayment.sendMax)
    { // make sure not over the max
        setResultConstraintNotMet();
        return false;
    }

    if (!updateSourceBalance(ltx, maxAmountRecv, bypassIssuerCheck,
                             doesSourceAccountExist))
    {
        return false;
    }
    return true;
}

bool
PathPaymentStrictReceiveOpFrame::doCheckValid(uint32_t ledgerVersion)
{
    if (mPathPayment.destAmount <= 0 || mPathPayment.sendMax <= 0)
    {
        setResultMalformed();
        return false;
    }
    if (!isAssetValid(mPathPayment.sendAsset) ||
        !isAssetValid(mPathPayment.destAsset))
    {
        setResultMalformed();
        return false;
    }
    auto const& p = mPathPayment.path;
    if (!std::all_of(p.begin(), p.end(), isAssetValid))
    {
        setResultMalformed();
        return false;
    }
    return true;
}

bool
PathPaymentStrictReceiveOpFrame::checkTransfer(int64_t maxSend,
                                               int64_t amountSend,
                                               int64_t maxRecv,
                                               int64_t amountRecv) const
{
    return maxRecv == amountRecv;
}

Asset const&
PathPaymentStrictReceiveOpFrame::getSourceAsset() const
{
    return mPathPayment.sendAsset;
}

Asset const&
PathPaymentStrictReceiveOpFrame::getDestAsset() const
{
    return mPathPayment.destAsset;
}

MuxedAccount const&
PathPaymentStrictReceiveOpFrame::getDestMuxedAccount() const
{
    return mPathPayment.destination;
}

xdr::xvector<Asset, 5> const&
PathPaymentStrictReceiveOpFrame::getPath() const
{
    return mPathPayment.path;
}

void
PathPaymentStrictReceiveOpFrame::setResultSuccess()
{
    innerResult().code(PATH_PAYMENT_STRICT_RECEIVE_SUCCESS);
}

void
PathPaymentStrictReceiveOpFrame::setResultMalformed()
{
    innerResult().code(PATH_PAYMENT_STRICT_RECEIVE_MALFORMED);
}

void
PathPaymentStrictReceiveOpFrame::setResultUnderfunded()
{
    innerResult().code(PATH_PAYMENT_STRICT_RECEIVE_UNDERFUNDED);
}

void
PathPaymentStrictReceiveOpFrame::setResultSourceNoTrust()
{
    innerResult().code(PATH_PAYMENT_STRICT_RECEIVE_SRC_NO_TRUST);
}

void
PathPaymentStrictReceiveOpFrame::setResultSourceNotAuthorized()
{
    innerResult().code(PATH_PAYMENT_STRICT_RECEIVE_SRC_NOT_AUTHORIZED);
}

void
PathPaymentStrictReceiveOpFrame::setResultNoDest()
{
    innerResult().code(PATH_PAYMENT_STRICT_RECEIVE_NO_DESTINATION);
}

void
PathPaymentStrictReceiveOpFrame::setResultDestNoTrust()
{
    innerResult().code(PATH_PAYMENT_STRICT_RECEIVE_NO_TRUST);
}

void
PathPaymentStrictReceiveOpFrame::setResultDestNotAuthorized()
{
    innerResult().code(PATH_PAYMENT_STRICT_RECEIVE_NOT_AUTHORIZED);
}

void
PathPaymentStrictReceiveOpFrame::setResultLineFull()
{
    innerResult().code(PATH_PAYMENT_STRICT_RECEIVE_LINE_FULL);
}

void
PathPaymentStrictReceiveOpFrame::setResultNoIssuer(Asset const& asset)
{
    innerResult().code(PATH_PAYMENT_STRICT_RECEIVE_NO_ISSUER);
    innerResult().noIssuer() = asset;
}

void
PathPaymentStrictReceiveOpFrame::setResultTooFewOffers()
{
    innerResult().code(PATH_PAYMENT_STRICT_RECEIVE_TOO_FEW_OFFERS);
}

void
PathPaymentStrictReceiveOpFrame::setResultOfferCrossSelf()
{
    innerResult().code(PATH_PAYMENT_STRICT_RECEIVE_OFFER_CROSS_SELF);
}

void
PathPaymentStrictReceiveOpFrame::setResultConstraintNotMet()
{
    innerResult().code(PATH_PAYMENT_STRICT_RECEIVE_OVER_SENDMAX);
}
}
