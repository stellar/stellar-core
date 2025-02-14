// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/PathPaymentStrictSendOpFrame.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "ledger/TrustLineWrapper.h"
#include "transactions/TransactionUtils.h"
#include "util/ProtocolVersion.h"
#include "util/XDROperators.h"
#include <Tracy.hpp>

#include <algorithm>
#include <ledger/LedgerHashUtils.h>

namespace stellar
{

PathPaymentStrictSendOpFrame::PathPaymentStrictSendOpFrame(
    Operation const& op, TransactionFrame const& parentTx)
    : PathPaymentOpFrameBase(op, parentTx)
    , mPathPayment(mOperation.body.pathPaymentStrictSendOp())
{
}

bool
PathPaymentStrictSendOpFrame::isOpSupported(LedgerHeader const& header) const
{
    return protocolVersionStartsFrom(header.ledgerVersion,
                                     ProtocolVersion::V_12);
}

bool
PathPaymentStrictSendOpFrame::doApply(
    AppConnector& app, AbstractLedgerTxn& ltx, Hash const& sorobanBasePrngSeed,
    OperationResult& res, std::shared_ptr<SorobanTxData> sorobanData) const
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

    setResultSuccess(res);

    bool bypassIssuerCheck = shouldBypassIssuerCheck(mPathPayment.path);
    if (!bypassIssuerCheck)
    {
        if (!stellar::loadAccountWithoutRecord(ltx, getDestID()))
        {
            setResultNoDest(res);
            return false;
        }
    }

    if (!updateSourceBalance(ltx, res, mPathPayment.sendAmount,
                             bypassIssuerCheck, true))
    {
        return false;
    }

    // build the full path to the destination, ending with destAsset
    std::vector<Asset> fullPath;
    fullPath.insert(fullPath.end(), mPathPayment.path.begin(),
                    mPathPayment.path.end());
    fullPath.emplace_back(getDestAsset());

    SHA256 sourceAndPathHasher;
    auto sourceAssetHash = getAssetHash(getSourceAsset());
    sourceAndPathHasher.add(
        ByteSlice(reinterpret_cast<unsigned char*>(&sourceAssetHash),
                  sizeof(sourceAssetHash)));
    for (auto const& asset : fullPath)
    {
        auto hash = getAssetHash(asset);
        sourceAndPathHasher.add(
            ByteSlice(reinterpret_cast<unsigned char*>(&hash), sizeof(hash)));
    }

    auto fullPathHash = sourceAndPathHasher.finish();

    auto iter =
        app.getLedgerManager().getPathPaymentStrictSendCache(fullPathHash);
    if (iter != app.getLedgerManager().getPathPaymentStrictSendCacheEnd())
    {
        auto const& sendAmountToMinReceiveAmount = iter->second;

        // Get set of receive amounts for which sendAmount is greater than or
        // equal to our send amount
        auto sendToReceiveAmountsIter = std::lower_bound(
            sendAmountToMinReceiveAmount.begin(),
            sendAmountToMinReceiveAmount.end(), mPathPayment.sendAmount,
            [](const auto& pair, uint64_t value) {
                // Pair == {sendAmount, minReceiveAmount}
                return pair.first < value;
            });

        // For each op that has sent the same or more than this op
        for (; sendToReceiveAmountsIter != sendAmountToMinReceiveAmount.end();
             ++sendToReceiveAmountsIter)
        {
            releaseAssert(sendToReceiveAmountsIter->first >=
                          mPathPayment.sendAmount);

            // If minimum received amount is less than or equal to destMin,
            // we know the trade will fail since a previous trade sent more and
            // received less than this op but still failed
            if (sendToReceiveAmountsIter->second <= mPathPayment.destMin)
            {
                setResultConstraintNotMet(res);
                pathStr += "-> hit";
                ZoneTextV(applyZone, pathStr.c_str(), pathStr.size());
                return false;
            }
        }
    }

    // Walk the path
    Asset sendAsset = getSourceAsset();
    int64_t maxAmountSend = mPathPayment.sendAmount;
    for (auto const& recvAsset : fullPath)
    {
        if (recvAsset == sendAsset)
        {
            continue;
        }

        if (!checkIssuer(ltx, recvAsset, res))
        {
            return false;
        }

        size_t offersCrossed = innerResult(res).success().offers.size();
        // offersCrossed will never be bigger than INT64_MAX because
        // - the machine would have run out of memory
        // - the limit, which cannot exceed INT64_MAX, should be enforced
        // so this subtraction is safe because getMaxOffersToCross() >= 0
        int64_t maxOffersToCross = getMaxOffersToCross() - offersCrossed;

        int64_t amountSend = 0;
        int64_t amountRecv = 0;
        std::vector<ClaimAtom> offerTrail;
        if (!convert(ltx, maxOffersToCross, sendAsset, maxAmountSend,
                     amountSend, recvAsset, INT64_MAX, amountRecv,
                     RoundingType::PATH_PAYMENT_STRICT_SEND, offerTrail, res))
        {
            return false;
        }

        maxAmountSend = amountRecv;
        sendAsset = recvAsset;

        // add offers that got taken on the way
        // insert in back to match the path's order
        auto& offers = innerResult(res).success().offers;
        offers.insert(offers.end(), offerTrail.begin(), offerTrail.end());
    }

    if (maxAmountSend < mPathPayment.destMin)
    {
        setResultConstraintNotMet(res);

        // Bound as much as possible. mPathPayment.destMin failed, but so would
        // maxAmountSend + 1.
        app.getLedgerManager().cachePathPaymentStrictSendFailure(
            fullPathHash, mPathPayment.sendAmount, maxAmountSend + 1,
            getSourceAsset(), fullPath);

        pathStr += "-> miss";
        ZoneTextV(applyZone, pathStr.c_str(), pathStr.size());
        return false;
    }

    if (!updateDestBalance(ltx, maxAmountSend, bypassIssuerCheck, res))
    {

        pathStr += "-> miss";
        ZoneTextV(applyZone, pathStr.c_str(), pathStr.size());
        return false;
    }

    // Invalidate caches for filled offers, but in reverse because counter party
    // is getting better. We don't need to invalidate paths we filled, as
    // filling them made the path strictly worse
    app.getLedgerManager().invalidatePathPaymentCachesForAssetPair(
        AssetPair{getSourceAsset(), fullPath.front()});

    for (size_t i = 0; i < fullPath.size() - 1; i++)
    {
        app.getLedgerManager().invalidatePathPaymentCachesForAssetPair(
            AssetPair{fullPath[i], fullPath[i + 1]});
    }

    pathStr += "-> miss";
    ZoneTextV(applyZone, pathStr.c_str(), pathStr.size());
    innerResult(res).success().last =
        SimplePaymentResult(getDestID(), getDestAsset(), maxAmountSend);
    return true;
}

bool
PathPaymentStrictSendOpFrame::doCheckValid(uint32_t ledgerVersion,
                                           OperationResult& res) const
{
    if (mPathPayment.sendAmount <= 0 || mPathPayment.destMin <= 0)
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
PathPaymentStrictSendOpFrame::setResultSuccess(OperationResult& res) const
{
    innerResult(res).code(PATH_PAYMENT_STRICT_SEND_SUCCESS);
}

void
PathPaymentStrictSendOpFrame::setResultMalformed(OperationResult& res) const
{
    innerResult(res).code(PATH_PAYMENT_STRICT_SEND_MALFORMED);
}

void
PathPaymentStrictSendOpFrame::setResultUnderfunded(OperationResult& res) const
{
    innerResult(res).code(PATH_PAYMENT_STRICT_SEND_UNDERFUNDED);
}

void
PathPaymentStrictSendOpFrame::setResultSourceNoTrust(OperationResult& res) const
{
    innerResult(res).code(PATH_PAYMENT_STRICT_SEND_SRC_NO_TRUST);
}

void
PathPaymentStrictSendOpFrame::setResultSourceNotAuthorized(
    OperationResult& res) const
{
    innerResult(res).code(PATH_PAYMENT_STRICT_SEND_SRC_NOT_AUTHORIZED);
}

void
PathPaymentStrictSendOpFrame::setResultNoDest(OperationResult& res) const
{
    innerResult(res).code(PATH_PAYMENT_STRICT_SEND_NO_DESTINATION);
}

void
PathPaymentStrictSendOpFrame::setResultDestNoTrust(OperationResult& res) const
{
    innerResult(res).code(PATH_PAYMENT_STRICT_SEND_NO_TRUST);
}

void
PathPaymentStrictSendOpFrame::setResultDestNotAuthorized(
    OperationResult& res) const
{
    innerResult(res).code(PATH_PAYMENT_STRICT_SEND_NOT_AUTHORIZED);
}

void
PathPaymentStrictSendOpFrame::setResultLineFull(OperationResult& res) const
{
    innerResult(res).code(PATH_PAYMENT_STRICT_SEND_LINE_FULL);
}

void
PathPaymentStrictSendOpFrame::setResultNoIssuer(Asset const& asset,
                                                OperationResult& res) const
{
    innerResult(res).code(PATH_PAYMENT_STRICT_SEND_NO_ISSUER);
    innerResult(res).noIssuer() = asset;
}

void
PathPaymentStrictSendOpFrame::setResultTooFewOffers(OperationResult& res) const
{
    innerResult(res).code(PATH_PAYMENT_STRICT_SEND_TOO_FEW_OFFERS);
}

void
PathPaymentStrictSendOpFrame::setResultOfferCrossSelf(
    OperationResult& res) const
{
    innerResult(res).code(PATH_PAYMENT_STRICT_SEND_OFFER_CROSS_SELF);
}

void
PathPaymentStrictSendOpFrame::setResultConstraintNotMet(
    OperationResult& res) const
{
    innerResult(res).code(PATH_PAYMENT_STRICT_SEND_UNDER_DESTMIN);
}
}
