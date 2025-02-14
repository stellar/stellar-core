// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/PathPaymentStrictReceiveOpFrame.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "ledger/TrustLineWrapper.h"
#include "transactions/TransactionUtils.h"
#include "util/ProtocolVersion.h"
#include "util/XDROperators.h"
#include <Tracy.hpp>

namespace stellar
{

PathPaymentStrictReceiveOpFrame::PathPaymentStrictReceiveOpFrame(
    Operation const& op, TransactionFrame const& parentTx)
    : PathPaymentOpFrameBase(op, parentTx)
    , mPathPayment(mOperation.body.pathPaymentStrictReceiveOp())
{
}

bool
PathPaymentStrictReceiveOpFrame::doApply(
    AppConnector& app, AbstractLedgerTxn& ltx, Hash const& sorobanBasePrngSeed,
    OperationResult& res, std::shared_ptr<SorobanTxData> sorobanData) const
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

    setResultSuccess(res);

    bool doesSourceAccountExist = true;
    if (protocolVersionIsBefore(ltx.loadHeader().current().ledgerVersion,
                                ProtocolVersion::V_8))
    {
        doesSourceAccountExist =
            (bool)stellar::loadAccountWithoutRecord(ltx, getSourceID());
    }

    bool bypassIssuerCheck = shouldBypassIssuerCheck(mPathPayment.path);
    if (!bypassIssuerCheck)
    {
        if (!stellar::loadAccountWithoutRecord(ltx, getDestID()))
        {
            setResultNoDest(res);
            return false;
        }
    }

    if (!updateDestBalance(ltx, mPathPayment.destAmount, bypassIssuerCheck,
                           res))
    {
        return false;
    }
    innerResult(res).success().last = SimplePaymentResult(
        getDestID(), getDestAsset(), mPathPayment.destAmount);

    // build the full path from the destination, ending with sendAsset
    std::vector<Asset> fullPath;
    fullPath.insert(fullPath.end(), mPathPayment.path.rbegin(),
                    mPathPayment.path.rend());
    fullPath.emplace_back(getSourceAsset());

    SHA256 pathHasher;
    for (auto iter = fullPath.rbegin(); iter != fullPath.rend(); iter++)
    {
        auto hash = getAssetHash(*iter);
        pathHasher.add(
            ByteSlice(reinterpret_cast<unsigned char*>(&hash), sizeof(hash)));
    }

    auto destHash = getAssetHash(getDestAsset());
    pathHasher.add(ByteSlice(reinterpret_cast<unsigned char*>(&destHash),
                             sizeof(destHash)));

    auto pathHash = pathHasher.finish();

    // TODO: Optimize better by keeping max as well
    auto iter = app.getLedgerManager().getPathPaymentStrictSendCache(pathHash);
    if (iter != app.getLedgerManager().getPathPaymentStrictSendCacheEnd())
    {
        auto const& sendAmountToMinReceiveAmount = iter->second;

        // Get set of receive amounts for which sendAmount is greater than or
        // equal to our send amount
        auto sendToReceiveAmountsIter = std::lower_bound(
            sendAmountToMinReceiveAmount.begin(),
            sendAmountToMinReceiveAmount.end(), mPathPayment.sendMax,
            [](const auto& pair, uint64_t value) {
                // Pair == {sendAmount, minReceiveAmount}
                return pair.first < value;
            });

        // For each op that has sent the same or more than this op
        for (; sendToReceiveAmountsIter != sendAmountToMinReceiveAmount.end();
             ++sendToReceiveAmountsIter)
        {
            releaseAssert(sendToReceiveAmountsIter->first >=
                          mPathPayment.sendMax);

            // If minimum received amount is less than or equal to destAmount,
            // we know the trade will fail since a previous trade sent more and
            // received less than this op but still failed
            if (sendToReceiveAmountsIter->second <= mPathPayment.destAmount)
            {
                setResultConstraintNotMet(res);
                pathStr += "-> hit";
                ZoneTextV(applyZone, pathStr.c_str(), pathStr.size());
                return false;
            }
        }
    }

    // Walk the path
    Asset recvAsset = getDestAsset();
    int64_t maxAmountRecv = mPathPayment.destAmount;
    for (auto const& sendAsset : fullPath)
    {
        if (recvAsset == sendAsset)
        {
            continue;
        }

        if (!checkIssuer(ltx, sendAsset, res))
        {
            return false;
        }

        int64_t maxOffersToCross = INT64_MAX;
        if (protocolVersionStartsFrom(
                ltx.loadHeader().current().ledgerVersion,
                FIRST_PROTOCOL_SUPPORTING_OPERATION_LIMITS))
        {
            size_t offersCrossed = innerResult(res).success().offers.size();
            // offersCrossed will never be bigger than INT64_MAX because
            // - the machine would have run out of memory
            // - the limit, which cannot exceed INT64_MAX, should be enforced
            // so this subtraction is safe because getMaxOffersToCross() >= 0
            maxOffersToCross = getMaxOffersToCross() - offersCrossed;
        }

        int64_t amountSend = 0;
        int64_t amountRecv = 0;
        std::vector<ClaimAtom> offerTrail;
        if (!convert(ltx, maxOffersToCross, sendAsset, INT64_MAX, amountSend,
                     recvAsset, maxAmountRecv, amountRecv,
                     RoundingType::PATH_PAYMENT_STRICT_RECEIVE, offerTrail,
                     res))
        {
            return false;
        }

        maxAmountRecv = amountSend;
        recvAsset = sendAsset;

        // add offers that got taken on the way
        // insert in front to match the path's order
        auto& offers = innerResult(res).success().offers;
        offers.insert(offers.begin(), offerTrail.begin(), offerTrail.end());
    }

    if (maxAmountRecv > mPathPayment.sendMax)
    { // make sure not over the max

        // Convert to strict send format for cache purposes
        std::vector<Asset> path;
        path.insert(path.end(), mPathPayment.path.begin(),
                    mPathPayment.path.end());
        path.emplace_back(getDestAsset());
        app.getLedgerManager().cachePathPaymentStrictSendFailure(
            pathHash, mPathPayment.sendMax, mPathPayment.destAmount,
            getSourceAsset(), path);

        setResultConstraintNotMet(res);
        return false;
    }

    if (!updateSourceBalance(ltx, res, maxAmountRecv, bypassIssuerCheck,
                             doesSourceAccountExist))
    {
        return false;
    }

    // Invalidate paths containing any asset pairs in the path, but reversed
    // since the counter party is getting better
    app.getLedgerManager().invalidatePathPaymentCachesForAssetPair(
        AssetPair{fullPath.front(), getDestAsset()});

    for (size_t i = 0; i < fullPath.size() - 1; i++)
    {
        app.getLedgerManager().invalidatePathPaymentCachesForAssetPair(
            AssetPair{fullPath[i + 1], fullPath[i]});
    }

    return true;
}

bool
PathPaymentStrictReceiveOpFrame::doCheckValid(uint32_t ledgerVersion,
                                              OperationResult& res) const
{
    if (mPathPayment.destAmount <= 0 || mPathPayment.sendMax <= 0)
    {
        setResultMalformed(res);
        return false;
    }
    if (!isAssetValid(mPathPayment.sendAsset, ledgerVersion) ||
        !isAssetValid(mPathPayment.destAsset, ledgerVersion))
    {
        setResultMalformed(res);
        return false;
    }
    for (auto const& p : mPathPayment.path)
    {
        if (!isAssetValid(p, ledgerVersion))
        {
            setResultMalformed(res);
            return false;
        }
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
PathPaymentStrictReceiveOpFrame::setResultSuccess(OperationResult& res) const
{
    innerResult(res).code(PATH_PAYMENT_STRICT_RECEIVE_SUCCESS);
}

void
PathPaymentStrictReceiveOpFrame::setResultMalformed(OperationResult& res) const
{
    innerResult(res).code(PATH_PAYMENT_STRICT_RECEIVE_MALFORMED);
}

void
PathPaymentStrictReceiveOpFrame::setResultUnderfunded(
    OperationResult& res) const
{
    innerResult(res).code(PATH_PAYMENT_STRICT_RECEIVE_UNDERFUNDED);
}

void
PathPaymentStrictReceiveOpFrame::setResultSourceNoTrust(
    OperationResult& res) const
{
    innerResult(res).code(PATH_PAYMENT_STRICT_RECEIVE_SRC_NO_TRUST);
}

void
PathPaymentStrictReceiveOpFrame::setResultSourceNotAuthorized(
    OperationResult& res) const
{
    innerResult(res).code(PATH_PAYMENT_STRICT_RECEIVE_SRC_NOT_AUTHORIZED);
}

void
PathPaymentStrictReceiveOpFrame::setResultNoDest(OperationResult& res) const
{
    innerResult(res).code(PATH_PAYMENT_STRICT_RECEIVE_NO_DESTINATION);
}

void
PathPaymentStrictReceiveOpFrame::setResultDestNoTrust(
    OperationResult& res) const
{
    innerResult(res).code(PATH_PAYMENT_STRICT_RECEIVE_NO_TRUST);
}

void
PathPaymentStrictReceiveOpFrame::setResultDestNotAuthorized(
    OperationResult& res) const
{
    innerResult(res).code(PATH_PAYMENT_STRICT_RECEIVE_NOT_AUTHORIZED);
}

void
PathPaymentStrictReceiveOpFrame::setResultLineFull(OperationResult& res) const
{
    innerResult(res).code(PATH_PAYMENT_STRICT_RECEIVE_LINE_FULL);
}

void
PathPaymentStrictReceiveOpFrame::setResultNoIssuer(Asset const& asset,
                                                   OperationResult& res) const
{
    innerResult(res).code(PATH_PAYMENT_STRICT_RECEIVE_NO_ISSUER);
    innerResult(res).noIssuer() = asset;
}

void
PathPaymentStrictReceiveOpFrame::setResultTooFewOffers(
    OperationResult& res) const
{
    innerResult(res).code(PATH_PAYMENT_STRICT_RECEIVE_TOO_FEW_OFFERS);
}

void
PathPaymentStrictReceiveOpFrame::setResultOfferCrossSelf(
    OperationResult& res) const
{
    innerResult(res).code(PATH_PAYMENT_STRICT_RECEIVE_OFFER_CROSS_SELF);
}

void
PathPaymentStrictReceiveOpFrame::setResultConstraintNotMet(
    OperationResult& res) const
{
    innerResult(res).code(PATH_PAYMENT_STRICT_RECEIVE_OVER_SENDMAX);
}
}
