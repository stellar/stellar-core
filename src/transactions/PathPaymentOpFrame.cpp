// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "transactions/PathPaymentOpFrame.h"
#include "OfferExchange.h"
#include "database/Database.h"
#include "ledger/LedgerEntryReference.h"
#include "ledger/LedgerHeaderReference.h"
#include "ledger/LedgerState.h"
#include "transactions/TransactionUtils.h"
#include "util/Logging.h"
#include <algorithm>

#include "main/Application.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"

#include "ledger/AccountReference.h"
#include "ledger/OfferReference.h"
#include "ledger/TrustLineReference.h"

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
PathPaymentOpFrame::doApply(Application& app, LedgerState& ls)
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

    if (!bypassIssuerCheck)
    {
        auto destination = stellar::loadAccount(ls, mPathPayment.destination);
        if (!destination)
        {
            app.getMetrics()
                .NewMeter({"op-path-payment", "failure", "no-destination"},
                          "operation")
                .Mark();
            innerResult().code(PATH_PAYMENT_NO_DESTINATION);
            return false;
        }
        destination.forget(ls);
    }

    // update last balance in the chain
    if (curB.type() == ASSET_TYPE_NATIVE)
    {
        auto destination = stellar::loadAccount(ls, mPathPayment.destination);
        if (!destination.addBalance(curBReceived))
        {
            app.getMetrics()
                .NewMeter({"op-path-payment", "invalid", "balance-overflow"},
                          "operation")
                .Mark();
            innerResult().code(PATH_PAYMENT_MALFORMED);
            return false;
        }
        destination.invalidate();
    }
    else
    {
        auto destLine = loadTrustLine(ls, mPathPayment.destination, curB);
        if (!bypassIssuerCheck)
        {
            auto issuer = stellar::loadAccount(ls, getIssuer(curB));
            if (!issuer)
            {
                app.getMetrics()
                    .NewMeter({"op-path-payment", "failure", "no-issuer"},
                              "operation")
                    .Mark();
                innerResult().code(PATH_PAYMENT_NO_ISSUER);
                innerResult().noIssuer() = curB;
                return false;
            }
            issuer.forget(ls);
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

        destLine->invalidate();
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
            LedgerState lsIssuer(ls);
            auto issuer = stellar::loadAccount(lsIssuer, getIssuer(curA));
            if (!issuer)
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

        OfferExchange oe;

        // curA -> curB
        medida::MetricsRegistry& metrics = app.getMetrics();
        OfferExchange::ConvertResult r = oe.convertWithOffers(
            ls, curA, INT64_MAX, curASent, curB, curBReceived,
            actualCurBReceived,
            [this, &metrics](OfferReference o) {
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
        AccountReference sourceAccount;
        auto header = ls.loadHeader();
        if (getCurrentLedgerVersion(header) > 7)
        {
            sourceAccount = stellar::loadAccount(ls, getSourceID());
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
        else
        {
            sourceAccount = loadSourceAccount(ls, header);
        }

        int64_t minBalance = sourceAccount.getMinimumBalance(header);

        if ((sourceAccount.account().balance - curBSent) < minBalance)
        { // they don't have enough to send
            app.getMetrics()
                .NewMeter({"op-path-payment", "failure", "underfunded"},
                          "operation")
                .Mark();
            innerResult().code(PATH_PAYMENT_UNDERFUNDED);
            return false;
        }

        if (getCurrentLedgerVersion(header) < 8)
        {
            sourceAccount.forget(ls);
            auto thisAccount = stellar::loadAccountRaw(ls, getSourceID());
            if (!thisAccount)
            {
                throw std::runtime_error("modifying account that does not exist");
            }
            thisAccount->invalidate();
            sourceAccount = loadSourceAccount(ls, header);
        }
        header->invalidate();

        auto ok = sourceAccount.addBalance(-curBSent);
        assert(ok);
    }
    else
    {
        if (!bypassIssuerCheck)
        {
            // We need a LedgerState here since it is possible that changes to
            // issuer were already stored.
            LedgerState lsIssuer(ls);
            auto issuer = stellar::loadAccount(lsIssuer, getIssuer(curB));
            if (!issuer)
            {
                app.getMetrics()
                    .NewMeter({"op-path-payment", "failure", "no-issuer"},
                              "operation")
                    .Mark();
                innerResult().code(PATH_PAYMENT_NO_ISSUER);
                innerResult().noIssuer() = curB;
                return false;
            }
            lsIssuer.rollback();
        }
        auto sourceLine = loadTrustLine(ls, getSourceID(), curB);

        if (!sourceLine)
        {
            app.getMetrics()
                .NewMeter({"op-path-payment", "failure", "src-no-trust"},
                          "operation")
                .Mark();
            innerResult().code(PATH_PAYMENT_SRC_NO_TRUST);
            return false;
        }

        if (!sourceLine->isAuthorized())
        {
            app.getMetrics()
                .NewMeter({"op-path-payment", "failure", "src-not-authorized"},
                          "operation")
                .Mark();
            innerResult().code(PATH_PAYMENT_SRC_NOT_AUTHORIZED);
            return false;
        }

        if (!sourceLine->addBalance(-curBSent))
        {
            app.getMetrics()
                .NewMeter({"op-path-payment", "failure", "underfunded"},
                          "operation")
                .Mark();
            innerResult().code(PATH_PAYMENT_UNDERFUNDED);
            return false;
        }

        sourceLine->invalidate();
    }

    app.getMetrics()
        .NewMeter({"op-path-payment", "success", "apply"}, "operation")
        .Mark();

    return true;
}

bool
PathPaymentOpFrame::doCheckValid(Application& app, uint32_t ledgerVersion)
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
