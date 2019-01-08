// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "transactions/PathPaymentOpFrame.h"
#include "OfferExchange.h"
#include "database/Database.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "ledger/TrustLineWrapper.h"
#include "transactions/TransactionUtils.h"
#include "util/Logging.h"
#include "util/XDROperators.h"
#include <algorithm>

#include "main/Application.h"

namespace stellar
{

using namespace std;

PathPaymentOpFrame::PathPaymentOpFrame(Operation const& op,
                                       OperationResult& res,
                                       TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
    , mPathPayment(mOperation.body.pathPaymentOp())
{
}

bool
PathPaymentOpFrame::doApply(Application& app, AbstractLedgerTxn& ltx)
{
    innerResult().code(PATH_PAYMENT_SUCCESS);

    // tracks the last amount that was traded
    int64_t curBReceived = mPathPayment.destAmount;
    Asset curB = mPathPayment.destAsset;

    // update balances, walks backwards

    // build the full path to the destination, starting with sendAsset
    std::vector<Asset> fullPath;
    fullPath.emplace_back(mPathPayment.sendAsset);
    fullPath.insert(fullPath.end(), mPathPayment.path.begin(),
                    mPathPayment.path.end());

    bool bypassIssuerCheck = false;

    // if the payment doesn't involve intermediate accounts
    // and the destination is the issuer we don't bother
    // checking if the destination account even exist
    // so that it's always possible to send credits back to its issuer
    bypassIssuerCheck = (curB.type() != ASSET_TYPE_NATIVE) &&
                        (fullPath.size() == 1) &&
                        (mPathPayment.sendAsset == mPathPayment.destAsset) &&
                        (getIssuer(curB) == mPathPayment.destination);

    bool doesSourceAccountExist = true;
    if (ltx.loadHeader().current().ledgerVersion < 8)
    {
        doesSourceAccountExist =
            (bool)stellar::loadAccountWithoutRecord(ltx, getSourceID());
    }

    if (!bypassIssuerCheck)
    {
        if (!stellar::loadAccountWithoutRecord(ltx, mPathPayment.destination))
        {
            innerResult().code(PATH_PAYMENT_NO_DESTINATION);
            return false;
        }
    }

    // update last balance in the chain
    if (curB.type() == ASSET_TYPE_NATIVE)
    {
        auto destination = stellar::loadAccount(ltx, mPathPayment.destination);
        if (!addBalance(ltx.loadHeader(), destination, curBReceived))
        {
            innerResult().code(PATH_PAYMENT_MALFORMED);
            return false;
        }
    }
    else
    {
        if (!bypassIssuerCheck)
        {
            auto issuer =
                stellar::loadAccountWithoutRecord(ltx, getIssuer(curB));
            if (!issuer)
            {
                innerResult().code(PATH_PAYMENT_NO_ISSUER);
                innerResult().noIssuer() = curB;
                return false;
            }
        }

        auto destLine =
            stellar::loadTrustLine(ltx, mPathPayment.destination, curB);
        if (!destLine)
        {
            innerResult().code(PATH_PAYMENT_NO_TRUST);
            return false;
        }

        if (!destLine.isAuthorized())
        {
            innerResult().code(PATH_PAYMENT_NOT_AUTHORIZED);
            return false;
        }

        if (!destLine.addBalance(ltx.loadHeader(), curBReceived))
        {
            innerResult().code(PATH_PAYMENT_LINE_FULL);
            return false;
        }
    }

    innerResult().success().last =
        SimplePaymentResult(mPathPayment.destination, curB, curBReceived);

    // now, walk the path backwards
    for (int i = (int)fullPath.size() - 1; i >= 0; i--)
    {
        int64_t curASent, actualCurBReceived;
        Asset const& curA = fullPath[i];

        if (curA == curB)
        {
            continue;
        }

        if (curA.type() != ASSET_TYPE_NATIVE)
        {
            if (!stellar::loadAccountWithoutRecord(ltx, getIssuer(curA)))
            {
                innerResult().code(PATH_PAYMENT_NO_ISSUER);
                innerResult().noIssuer() = curA;
                return false;
            }
        }

        // curA -> curB
        std::vector<ClaimOfferAtom> offerTrail;
        ConvertResult r = convertWithOffers(
            ltx, curA, INT64_MAX, curASent, curB, curBReceived,
            actualCurBReceived, true,
            [this](LedgerTxnEntry const& o) {
                auto const& offer = o.current().data.offer();
                if (offer.sellerID == getSourceID())
                {
                    // we are crossing our own offer
                    innerResult().code(PATH_PAYMENT_OFFER_CROSS_SELF);
                    return OfferFilterResult::eStop;
                }
                return OfferFilterResult::eKeep;
            },
            offerTrail);

        assert(curASent >= 0);

        switch (r)
        {
        case ConvertResult::eFilterStop:
            return false;
        case ConvertResult::eOK:
            if (curBReceived == actualCurBReceived)
            {
                break;
            }
        // fall through
        case ConvertResult::ePartial:
            innerResult().code(PATH_PAYMENT_TOO_FEW_OFFERS);
            return false;
        }
        assert(curBReceived == actualCurBReceived);
        curBReceived = curASent; // next round, we need to send enough
        curB = curA;

        // add offers that got taken on the way
        // insert in front to match the path's order
        auto& offers = innerResult().success().offers;
        offers.insert(offers.begin(), offerTrail.begin(), offerTrail.end());
    }

    // last step: we've reached the first account in the chain, update its
    // balance
    int64_t curBSent = curBReceived;
    if (curBSent > mPathPayment.sendMax)
    { // make sure not over the max
        innerResult().code(PATH_PAYMENT_OVER_SENDMAX);
        return false;
    }

    if (curB.type() == ASSET_TYPE_NATIVE)
    {
        auto header = ltx.loadHeader();
        LedgerTxnEntry sourceAccount;
        if (header.current().ledgerVersion > 7)
        {
            sourceAccount = stellar::loadAccount(ltx, getSourceID());
            if (!sourceAccount)
            {
                innerResult().code(PATH_PAYMENT_MALFORMED);
                return false;
            }
        }
        else
        {
            sourceAccount = loadSourceAccount(ltx, header);
        }

        if (curBSent > getAvailableBalance(header, sourceAccount))
        { // they don't have enough to send
            innerResult().code(PATH_PAYMENT_UNDERFUNDED);
            return false;
        }

        if (!doesSourceAccountExist)
        {
            throw std::runtime_error("modifying account that does not exist");
        }

        auto ok = addBalance(header, sourceAccount, -curBSent);
        assert(ok);
    }
    else
    {
        if (!bypassIssuerCheck)
        {
            auto issuer =
                stellar::loadAccountWithoutRecord(ltx, getIssuer(curB));
            if (!issuer)
            {
                innerResult().code(PATH_PAYMENT_NO_ISSUER);
                innerResult().noIssuer() = curB;
                return false;
            }
        }

        auto sourceLine = loadTrustLine(ltx, getSourceID(), curB);
        if (!sourceLine)
        {
            innerResult().code(PATH_PAYMENT_SRC_NO_TRUST);
            return false;
        }

        if (!sourceLine.isAuthorized())
        {
            innerResult().code(PATH_PAYMENT_SRC_NOT_AUTHORIZED);
            return false;
        }

        if (!sourceLine.addBalance(ltx.loadHeader(), -curBSent))
        {
            innerResult().code(PATH_PAYMENT_UNDERFUNDED);
            return false;
        }
    }

    return true;
}

bool
PathPaymentOpFrame::doCheckValid(Application& app, uint32_t ledgerVersion)
{
    if (mPathPayment.destAmount <= 0 || mPathPayment.sendMax <= 0)
    {
        innerResult().code(PATH_PAYMENT_MALFORMED);
        return false;
    }
    if (!isAssetValid(mPathPayment.sendAsset) ||
        !isAssetValid(mPathPayment.destAsset))
    {
        innerResult().code(PATH_PAYMENT_MALFORMED);
        return false;
    }
    auto const& p = mPathPayment.path;
    if (!std::all_of(p.begin(), p.end(), isAssetValid))
    {
        innerResult().code(PATH_PAYMENT_MALFORMED);
        return false;
    }
    return true;
}
}
