// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/PathPaymentOpFrame.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "ledger/TrustLineWrapper.h"
#include "transactions/TransactionUtils.h"
#include "util/XDROperators.h"

namespace stellar
{

PathPaymentOpFrame::PathPaymentOpFrame(Operation const& op,
                                       OperationResult& res,
                                       TransactionFrame& parentTx)
    : PathPaymentOpFrameBase(op, res, parentTx)
    , mPathPayment(mOperation.body.pathPaymentOp())
{
}

bool
PathPaymentOpFrame::doApply(AbstractLedgerTxn& ltx)
{
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
PathPaymentOpFrame::doCheckValid(uint32_t ledgerVersion)
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
PathPaymentOpFrame::checkTransfer(int64_t maxSend, int64_t amountSend,
                                  int64_t maxRecv, int64_t amountRecv) const
{
    return maxRecv == amountRecv;
}

Asset const&
PathPaymentOpFrame::getSourceAsset() const
{
    return mPathPayment.sendAsset;
}

Asset const&
PathPaymentOpFrame::getDestAsset() const
{
    return mPathPayment.destAsset;
}

AccountID const&
PathPaymentOpFrame::getDestID() const
{
    return mPathPayment.destination;
}

xdr::xvector<Asset, 5> const&
PathPaymentOpFrame::getPath() const
{
    return mPathPayment.path;
}

void
PathPaymentOpFrame::setResultSuccess()
{
    innerResult().code(PATH_PAYMENT_SUCCESS);
}

void
PathPaymentOpFrame::setResultMalformed()
{
    innerResult().code(PATH_PAYMENT_MALFORMED);
}

void
PathPaymentOpFrame::setResultUnderfunded()
{
    innerResult().code(PATH_PAYMENT_UNDERFUNDED);
}

void
PathPaymentOpFrame::setResultSourceNoTrust()
{
    innerResult().code(PATH_PAYMENT_SRC_NO_TRUST);
}

void
PathPaymentOpFrame::setResultSourceNotAuthorized()
{
    innerResult().code(PATH_PAYMENT_SRC_NOT_AUTHORIZED);
}

void
PathPaymentOpFrame::setResultNoDest()
{
    innerResult().code(PATH_PAYMENT_NO_DESTINATION);
}

void
PathPaymentOpFrame::setResultDestNoTrust()
{
    innerResult().code(PATH_PAYMENT_NO_TRUST);
}

void
PathPaymentOpFrame::setResultDestNotAuthorized()
{
    innerResult().code(PATH_PAYMENT_NOT_AUTHORIZED);
}

void
PathPaymentOpFrame::setResultLineFull()
{
    innerResult().code(PATH_PAYMENT_LINE_FULL);
}

void
PathPaymentOpFrame::setResultNoIssuer(Asset const& asset)
{
    innerResult().code(PATH_PAYMENT_NO_ISSUER);
    innerResult().noIssuer() = asset;
}

void
PathPaymentOpFrame::setResultTooFewOffers()
{
    innerResult().code(PATH_PAYMENT_TOO_FEW_OFFERS);
}

void
PathPaymentOpFrame::setResultOfferCrossSelf()
{
    innerResult().code(PATH_PAYMENT_OFFER_CROSS_SELF);
}

void
PathPaymentOpFrame::setResultConstraintNotMet()
{
    innerResult().code(PATH_PAYMENT_OVER_SENDMAX);
}
}
