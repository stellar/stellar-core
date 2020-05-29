// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/PathPaymentStrictSendOpFrame.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "ledger/TrustLineWrapper.h"
#include "transactions/TransactionUtils.h"
#include "util/XDROperators.h"
#include <Tracy.hpp>

namespace stellar
{

PathPaymentStrictSendOpFrame::PathPaymentStrictSendOpFrame(
    Operation const& op, OperationResult& res, TransactionFrame& parentTx)
    : PathPaymentOpFrameBase(op, res, parentTx)
    , mPathPayment(mOperation.body.pathPaymentStrictSendOp())
{
}

bool
PathPaymentStrictSendOpFrame::isVersionSupported(uint32_t protocolVersion) const
{
    return protocolVersion >= 12;
}

bool
PathPaymentStrictSendOpFrame::doApply(AbstractLedgerTxn& ltx)
{
    ZoneNamedN(applyZone, "PathPaymentStrictSendOp apply", true);
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

    bool bypassIssuerCheck = shouldBypassIssuerCheck(mPathPayment.path);
    if (!bypassIssuerCheck)
    {
        if (!stellar::loadAccountWithoutRecord(ltx, getDestID()))
        {
            setResultNoDest();
            return false;
        }
    }

    if (!updateSourceBalance(ltx, mPathPayment.sendAmount, bypassIssuerCheck,
                             true))
    {
        return false;
    }

    // build the full path to the destination, ending with destAsset
    std::vector<Asset> fullPath;
    fullPath.insert(fullPath.end(), mPathPayment.path.begin(),
                    mPathPayment.path.end());
    fullPath.emplace_back(getDestAsset());

    // Walk the path
    Asset sendAsset = getSourceAsset();
    int64_t maxAmountSend = mPathPayment.sendAmount;
    for (auto const& recvAsset : fullPath)
    {
        if (recvAsset == sendAsset)
        {
            continue;
        }

        if (!checkIssuer(ltx, recvAsset))
        {
            return false;
        }

        size_t offersCrossed = innerResult().success().offers.size();
        // offersCrossed will never be bigger than INT64_MAX because
        // - the machine would have run out of memory
        // - the limit, which cannot exceed INT64_MAX, should be enforced
        // so this subtraction is safe because MAX_OFFERS_TO_CROSS >= 0
        int64_t maxOffersToCross = MAX_OFFERS_TO_CROSS - offersCrossed;

        int64_t amountSend = 0;
        int64_t amountRecv = 0;
        std::vector<ClaimOfferAtom> offerTrail;
        if (!convert(ltx, maxOffersToCross, sendAsset, maxAmountSend,
                     amountSend, recvAsset, INT64_MAX, amountRecv,
                     RoundingType::PATH_PAYMENT_STRICT_SEND, offerTrail))
        {
            return false;
        }

        maxAmountSend = amountRecv;
        sendAsset = recvAsset;

        // add offers that got taken on the way
        // insert in back to match the path's order
        auto& offers = innerResult().success().offers;
        offers.insert(offers.end(), offerTrail.begin(), offerTrail.end());
    }

    if (maxAmountSend < mPathPayment.destMin)
    { // make sure not over the max
        setResultConstraintNotMet();
        return false;
    }

    if (!updateDestBalance(ltx, maxAmountSend, bypassIssuerCheck))
    {
        return false;
    }
    innerResult().success().last =
        SimplePaymentResult(getDestID(), getDestAsset(), maxAmountSend);
    return true;
}

bool
PathPaymentStrictSendOpFrame::doCheckValid(uint32_t ledgerVersion)
{
    if (mPathPayment.sendAmount <= 0 || mPathPayment.destMin <= 0)
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
PathPaymentStrictSendOpFrame::checkTransfer(int64_t maxSend, int64_t amountSend,
                                            int64_t maxRecv,
                                            int64_t amountRecv) const
{
    return maxSend == amountSend;
}

Asset const&
PathPaymentStrictSendOpFrame::getSourceAsset() const
{
    return mPathPayment.sendAsset;
}

Asset const&
PathPaymentStrictSendOpFrame::getDestAsset() const
{
    return mPathPayment.destAsset;
}

MuxedAccount const&
PathPaymentStrictSendOpFrame::getDestMuxedAccount() const
{
    return mPathPayment.destination;
}

xdr::xvector<Asset, 5> const&
PathPaymentStrictSendOpFrame::getPath() const
{
    return mPathPayment.path;
}

void
PathPaymentStrictSendOpFrame::setResultSuccess()
{
    innerResult().code(PATH_PAYMENT_STRICT_SEND_SUCCESS);
}

void
PathPaymentStrictSendOpFrame::setResultMalformed()
{
    innerResult().code(PATH_PAYMENT_STRICT_SEND_MALFORMED);
}

void
PathPaymentStrictSendOpFrame::setResultUnderfunded()
{
    innerResult().code(PATH_PAYMENT_STRICT_SEND_UNDERFUNDED);
}

void
PathPaymentStrictSendOpFrame::setResultSourceNoTrust()
{
    innerResult().code(PATH_PAYMENT_STRICT_SEND_SRC_NO_TRUST);
}

void
PathPaymentStrictSendOpFrame::setResultSourceNotAuthorized()
{
    innerResult().code(PATH_PAYMENT_STRICT_SEND_SRC_NOT_AUTHORIZED);
}

void
PathPaymentStrictSendOpFrame::setResultNoDest()
{
    innerResult().code(PATH_PAYMENT_STRICT_SEND_NO_DESTINATION);
}

void
PathPaymentStrictSendOpFrame::setResultDestNoTrust()
{
    innerResult().code(PATH_PAYMENT_STRICT_SEND_NO_TRUST);
}

void
PathPaymentStrictSendOpFrame::setResultDestNotAuthorized()
{
    innerResult().code(PATH_PAYMENT_STRICT_SEND_NOT_AUTHORIZED);
}

void
PathPaymentStrictSendOpFrame::setResultLineFull()
{
    innerResult().code(PATH_PAYMENT_STRICT_SEND_LINE_FULL);
}

void
PathPaymentStrictSendOpFrame::setResultNoIssuer(Asset const& asset)
{
    innerResult().code(PATH_PAYMENT_STRICT_SEND_NO_ISSUER);
    innerResult().noIssuer() = asset;
}

void
PathPaymentStrictSendOpFrame::setResultTooFewOffers()
{
    innerResult().code(PATH_PAYMENT_STRICT_SEND_TOO_FEW_OFFERS);
}

void
PathPaymentStrictSendOpFrame::setResultOfferCrossSelf()
{
    innerResult().code(PATH_PAYMENT_STRICT_SEND_OFFER_CROSS_SELF);
}

void
PathPaymentStrictSendOpFrame::setResultConstraintNotMet()
{
    innerResult().code(PATH_PAYMENT_STRICT_SEND_UNDER_DESTMIN);
}
}
