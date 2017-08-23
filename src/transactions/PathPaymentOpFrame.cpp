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

PathPaymentOpFrame::PathPaymentOpFrame(Operation const& op,
                                       OperationResult& res,
                                       TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
    , mPathPayment(mOperation.body.pathPaymentOp())
{
}

bool
PathPaymentOpFrame::doApply(Application& app, LedgerDelta& delta,
                            LedgerManager& ledgerManager)
{
    Database& db = ledgerManager.getDatabase();

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
            innerResult().code(PATH_PAYMENT_NO_DESTINATION);
            return false;
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
            innerResult().code(PATH_PAYMENT_MALFORMED);
            return false;
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
                innerResult().code(PATH_PAYMENT_NO_ISSUER);
                innerResult().noIssuer() = curB;
                return false;
            }
            destLine = tlI.first;
        }

        if (!destLine)
        {
            app.getMetrics()
                .NewMeter({"op-path-payment", "failure", "no-trust"},
                          "operation")
                .Mark();
            innerResult().code(PATH_PAYMENT_NO_TRUST);
            return false;
        }

        if (!destLine->isAuthorized())
        {
            app.getMetrics()
                .NewMeter({"op-path-payment", "failure", "not-authorized"},
                          "operation")
                .Mark();
            innerResult().code(PATH_PAYMENT_NOT_AUTHORIZED);
            return false;
        }

        if (!destLine->addBalance(curBReceived))
        {
            app.getMetrics()
                .NewMeter({"op-path-payment", "failure", "line-full"},
                          "operation")
                .Mark();
            innerResult().code(PATH_PAYMENT_LINE_FULL);
            return false;
        }

        destLine->storeChange(delta, db);
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
            if (!AccountFrame::loadAccount(delta, getIssuer(curA), db))
            {
                app.getMetrics()
                    .NewMeter({"op-path-payment", "failure", "no-issuer"},
                              "operation")
                    .Mark();
                innerResult().code(PATH_PAYMENT_NO_ISSUER);
                innerResult().noIssuer() = curA;
                return false;
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
                    // we are crossing our own offer, potentially invalidating
                    // mSourceAccount (balance or numSubEntries)
                    metrics
                        .NewMeter(
                            {"op-path-payment", "failure", "offer-cross-self"},
                            "operation")
                        .Mark();
                    innerResult().code(PATH_PAYMENT_OFFER_CROSS_SELF);
                    return OfferExchange::eStop;
                }
                return OfferExchange::eKeep;
            });

        assert(curASent >= 0);

        switch (r)
        {
        case OfferExchange::eFilterStop:
            return false;
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
            innerResult().code(PATH_PAYMENT_TOO_FEW_OFFERS);
            return false;
        }
        assert(curBReceived == actualCurBReceived);
        curBReceived = curASent; // next round, we need to send enough
        curB = curA;

        // add offers that got taken on the way
        // insert in front to match the path's order
        auto& offers = innerResult().success().offers;
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
        innerResult().code(PATH_PAYMENT_OVER_SENDMAX);
        return false;
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
                innerResult().code(PATH_PAYMENT_MALFORMED);
                return false;
            }
        }

        int64_t minBalance = sourceAccount->getMinimumBalance(ledgerManager);

        if ((sourceAccount->getAccount().balance - curBSent) < minBalance)
        { // they don't have enough to send
            app.getMetrics()
                .NewMeter({"op-path-payment", "failure", "underfunded"},
                          "operation")
                .Mark();
            innerResult().code(PATH_PAYMENT_UNDERFUNDED);
            return false;
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
                innerResult().code(PATH_PAYMENT_NO_ISSUER);
                innerResult().noIssuer() = curB;
                return false;
            }
            sourceLineFrame = tlI.first;
        }

        if (!sourceLineFrame)
        {
            app.getMetrics()
                .NewMeter({"op-path-payment", "failure", "src-no-trust"},
                          "operation")
                .Mark();
            innerResult().code(PATH_PAYMENT_SRC_NO_TRUST);
            return false;
        }

        if (!sourceLineFrame->isAuthorized())
        {
            app.getMetrics()
                .NewMeter({"op-path-payment", "failure", "src-not-authorized"},
                          "operation")
                .Mark();
            innerResult().code(PATH_PAYMENT_SRC_NOT_AUTHORIZED);
            return false;
        }

        if (!sourceLineFrame->addBalance(-curBSent))
        {
            app.getMetrics()
                .NewMeter({"op-path-payment", "failure", "underfunded"},
                          "operation")
                .Mark();
            innerResult().code(PATH_PAYMENT_UNDERFUNDED);
            return false;
        }

        sourceLineFrame->storeChange(delta, db);
    }

    app.getMetrics()
        .NewMeter({"op-path-payment", "success", "apply"}, "operation")
        .Mark();

    return true;
}

bool
PathPaymentOpFrame::doCheckValid(Application& app)
{
    if (mPathPayment.destAmount <= 0 || mPathPayment.sendMax <= 0)
    {
        app.getMetrics()
            .NewMeter({"op-path-payment", "invalid", "malformed-amounts"},
                      "operation")
            .Mark();
        innerResult().code(PATH_PAYMENT_MALFORMED);
        return false;
    }
    if (!isAssetValid(mPathPayment.sendAsset) ||
        !isAssetValid(mPathPayment.destAsset))
    {
        app.getMetrics()
            .NewMeter({"op-path-payment", "invalid", "malformed-currencies"},
                      "operation")
            .Mark();
        innerResult().code(PATH_PAYMENT_MALFORMED);
        return false;
    }
    auto const& p = mPathPayment.path;
    if (!std::all_of(p.begin(), p.end(), isAssetValid))
    {
        app.getMetrics()
            .NewMeter({"op-path-payment", "invalid", "malformed-currencies"},
                      "operation")
            .Mark();
        innerResult().code(PATH_PAYMENT_MALFORMED);
        return false;
    }
    return true;
}
}
