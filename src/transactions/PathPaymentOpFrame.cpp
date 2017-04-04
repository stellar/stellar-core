// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "transactions/PathPaymentOpFrame.h"
#include "OfferExchange.h"
#include "database/Database.h"
#include "ledger/LedgerDelta.h"
#include "ledger/OfferFrame.h"
#include "ledger/TrustFrame.h"
#include "util/Logging.h"
#include <algorithm>

#include "main/Application.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"

namespace stellar
{

using namespace std;
using xdr::operator==;

namespace
{

OperationResult
makeResult(PathPaymentResultCode code)
{
    auto result = OperationResult{};
    result.code(opINNER);
    result.tr().type(PATH_PAYMENT);
    result.tr().pathPaymentResult().code(code);
    return result;
}

OperationResult
makeSuccessResult(xdr::xvector<ClaimOfferAtom> const& offers,
                  SimplePaymentResult const& last)
{
    auto result = OperationResult{};
    result.code(opINNER);
    result.tr().type(PATH_PAYMENT);
    result.tr().pathPaymentResult().code(PATH_PAYMENT_SUCCESS);
    result.tr().pathPaymentResult().success().offers = offers;
    result.tr().pathPaymentResult().success().last = last;
    return result;
}

OperationResult
makeNoIssuerResult(Asset const& noIssuer)
{
    auto result = OperationResult{};
    result.code(opINNER);
    result.tr().type(PATH_PAYMENT);
    result.tr().pathPaymentResult().code(PATH_PAYMENT_NO_ISSUER);
    result.tr().pathPaymentResult().noIssuer() = noIssuer;
    return result;
}
}

PathPaymentOpFrame::PathPaymentOpFrame(Operation const& op,
                                       TransactionFrame& parentTx)
    : OperationFrame(op, parentTx)
    , mPathPayment(mOperation.body.pathPaymentOp())
{
}

OperationResult
PathPaymentOpFrame::doApply(Application& app, LedgerDelta& delta,
                            LedgerManager& ledgerManager)
{
    Database& db = ledgerManager.getDatabase();

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

    AccountFrame::pointer destination;

    if (!bypassIssuerCheck)
    {
        destination =
            AccountFrame::loadAccount(delta, mPathPayment.destination, db);

        if (!destination)
        {
            app.getMetrics()
                .NewMeter({"op-path-payment", "failure", "no-destination"},
                          "operation")
                .Mark();
            return makeResult(PATH_PAYMENT_NO_DESTINATION);
        }
    }

    // update last balance in the chain
    if (curB.type() == ASSET_TYPE_NATIVE)
    {
        if (!destination->addBalance(curBReceived))
        {
            app.getMetrics()
                .NewMeter({"op-path-payment", "invalid", "balance-overflow"},
                          "operation")
                .Mark();
            return makeResult(PATH_PAYMENT_MALFORMED);
        }
        destination->storeChange(delta, db);
    }
    else
    {
        TrustFrame::pointer destLine;

        if (bypassIssuerCheck)
        {
            destLine = TrustFrame::loadTrustLine(mPathPayment.destination, curB,
                                                 db, &delta);
        }
        else
        {
            auto tlI = TrustFrame::loadTrustLineIssuer(mPathPayment.destination,
                                                       curB, db, delta);
            if (!tlI.second)
            {
                app.getMetrics()
                    .NewMeter({"op-path-payment", "failure", "no-issuer"},
                              "operation")
                    .Mark();
                return makeNoIssuerResult(curB);
            }
            destLine = tlI.first;
        }

        if (!destLine)
        {
            app.getMetrics()
                .NewMeter({"op-path-payment", "failure", "no-trust"},
                          "operation")
                .Mark();
            return makeResult(PATH_PAYMENT_NO_TRUST);
        }

        if (!destLine->isAuthorized())
        {
            app.getMetrics()
                .NewMeter({"op-path-payment", "failure", "not-authorized"},
                          "operation")
                .Mark();
            return makeResult(PATH_PAYMENT_NOT_AUTHORIZED);
        }

        if (!destLine->addBalance(curBReceived))
        {
            app.getMetrics()
                .NewMeter({"op-path-payment", "failure", "line-full"},
                          "operation")
                .Mark();
            return makeResult(PATH_PAYMENT_LINE_FULL);
        }

        destLine->storeChange(delta, db);
    }

    auto last =
        SimplePaymentResult(mPathPayment.destination, curB, curBReceived);
    auto offers = xdr::xvector<ClaimOfferAtom>{};

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
            if (!AccountFrame::loadAccount(delta, getIssuer(curA), db))
            {
                app.getMetrics()
                    .NewMeter({"op-path-payment", "failure", "no-issuer"},
                              "operation")
                    .Mark();
                return makeNoIssuerResult(curA);
            }
        }

        OfferExchange oe(delta, ledgerManager);

        // curA -> curB
        medida::MetricsRegistry& metrics = app.getMetrics();
        OfferExchange::ConvertResult r = oe.convertWithOffers(
            curA, INT64_MAX, curASent, curB, curBReceived, actualCurBReceived,
            [this, &metrics](OfferFrame const& o) {
                if (o.getSellerID() == getSourceID())
                {
                    return OfferExchange::eStop;
                }
                return OfferExchange::eKeep;
            });

        assert(curASent >= 0);

        switch (r)
        {
        case OfferExchange::eFilterStop:
            // we are crossing our own offer, potentially invalidating
            // sourceAccount (balance or numSubEntries)
            metrics
                .NewMeter({"op-path-payment", "failure", "offer-cross-self"},
                          "operation")
                .Mark();
            return makeResult(PATH_PAYMENT_OFFER_CROSS_SELF);
        case OfferExchange::eOK:
            if (curBReceived == actualCurBReceived)
            {
                break;
            }
        // fall through
        case OfferExchange::ePartial:
            app.getMetrics()
                .NewMeter({"op-path-payment", "failure", "too-few-offers"},
                          "operation")
                .Mark();
            return makeResult(PATH_PAYMENT_TOO_FEW_OFFERS);
        }
        assert(curBReceived == actualCurBReceived);
        curBReceived = curASent; // next round, we need to send enough
        curB = curA;

        // add offers that got taken on the way
        // insert in front to match the path's order
        offers.insert(offers.begin(), oe.getOfferTrail().begin(),
                      oe.getOfferTrail().end());
    }

    // last step: we've reached the first account in the chain, update its
    // balance

    int64_t curBSent;

    curBSent = curBReceived;

    if (curBSent > mPathPayment.sendMax)
    { // make sure not over the max
        app.getMetrics()
            .NewMeter({"op-path-payment", "failure", "over-send-max"},
                      "operation")
            .Mark();
        return makeResult(PATH_PAYMENT_OVER_SENDMAX);
    }

    if (curB.type() == ASSET_TYPE_NATIVE)
    {
        auto sourceAccount = mSourceAccount;

        if (ledgerManager.getCurrentLedgerVersion() > 7)
        {
            sourceAccount =
                AccountFrame::loadAccount(delta, mSourceAccount->getID(), db);

            if (!sourceAccount)
            {
                app.getMetrics()
                    .NewMeter({"op-path-payment", "invalid", "no-account"},
                              "operation")
                    .Mark();
                return makeResult(PATH_PAYMENT_MALFORMED);
            }
        }

        int64_t minBalance = sourceAccount->getMinimumBalance(ledgerManager);

        if ((sourceAccount->getAccount().balance - curBSent) < minBalance)
        { // they don't have enough to send
            app.getMetrics()
                .NewMeter({"op-path-payment", "failure", "underfunded"},
                          "operation")
                .Mark();
            return makeResult(PATH_PAYMENT_UNDERFUNDED);
        }

        auto ok = sourceAccount->addBalance(-curBSent);
        assert(ok);
        sourceAccount->storeChange(delta, db);
    }
    else
    {
        TrustFrame::pointer sourceLineFrame;
        if (bypassIssuerCheck)
        {
            sourceLineFrame =
                TrustFrame::loadTrustLine(getSourceID(), curB, db, &delta);
        }
        else
        {
            auto tlI =
                TrustFrame::loadTrustLineIssuer(getSourceID(), curB, db, delta);

            if (!tlI.second)
            {
                app.getMetrics()
                    .NewMeter({"op-path-payment", "failure", "no-issuer"},
                              "operation")
                    .Mark();
                return makeNoIssuerResult(curB);
            }
            sourceLineFrame = tlI.first;
        }

        if (!sourceLineFrame)
        {
            app.getMetrics()
                .NewMeter({"op-path-payment", "failure", "src-no-trust"},
                          "operation")
                .Mark();
            return makeResult(PATH_PAYMENT_SRC_NO_TRUST);
        }

        if (!sourceLineFrame->isAuthorized())
        {
            app.getMetrics()
                .NewMeter({"op-path-payment", "failure", "src-not-authorized"},
                          "operation")
                .Mark();
            return makeResult(PATH_PAYMENT_SRC_NOT_AUTHORIZED);
        }

        if (!sourceLineFrame->addBalance(-curBSent))
        {
            app.getMetrics()
                .NewMeter({"op-path-payment", "failure", "underfunded"},
                          "operation")
                .Mark();
            return makeResult(PATH_PAYMENT_UNDERFUNDED);
        }

        sourceLineFrame->storeChange(delta, db);
    }

    app.getMetrics()
        .NewMeter({"op-path-payment", "success", "apply"}, "operation")
        .Mark();

    return makeSuccessResult(offers, last);
}

OperationResult
PathPaymentOpFrame::doCheckValid(Application& app)
{
    if (mPathPayment.destAmount <= 0 || mPathPayment.sendMax <= 0)
    {
        app.getMetrics()
            .NewMeter({"op-path-payment", "invalid", "malformed-amounts"},
                      "operation")
            .Mark();
        return makeResult(PATH_PAYMENT_MALFORMED);
    }
    if (!isAssetValid(mPathPayment.sendAsset) ||
        !isAssetValid(mPathPayment.destAsset))
    {
        app.getMetrics()
            .NewMeter({"op-path-payment", "invalid", "malformed-currencies"},
                      "operation")
            .Mark();
        return makeResult(PATH_PAYMENT_MALFORMED);
    }
    auto const& p = mPathPayment.path;
    if (!std::all_of(p.begin(), p.end(), isAssetValid))
    {
        app.getMetrics()
            .NewMeter({"op-path-payment", "invalid", "malformed-currencies"},
                      "operation")
            .Mark();
        return makeResult(PATH_PAYMENT_MALFORMED);
    }
    return makeResult(PATH_PAYMENT_SUCCESS);
}
}
