// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/PathPaymentOpFrame.h"
#include "util/Logging.h"
#include "ledger/LedgerDelta.h"
#include "ledger/TrustFrame.h"
#include "ledger/OfferFrame.h"
#include "database/Database.h"
#include "OfferExchange.h"
#include <algorithm>

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
PathPaymentOpFrame::doApply(LedgerDelta& delta, LedgerManager& ledgerManager)
{
    AccountFrame destination;

    Database& db = ledgerManager.getDatabase();

    if (!AccountFrame::loadAccount(mPathPayment.destination, destination, db))
    {
        innerResult().code(PATH_PAYMENT_NO_DESTINATION);
        return false;
    }

    innerResult().code(PATH_PAYMENT_SUCCESS);

    // tracks the last amount that was traded
    int64_t curBReceived = mPathPayment.destAmount;
    Currency curB = mPathPayment.destCurrency;

    // update balances, walks backwards

    // build the full path to the destination, starting with sendCurrency
    std::vector<Currency> fullPath;
    fullPath.emplace_back(mPathPayment.sendCurrency);
    fullPath.insert(fullPath.end(), mPathPayment.path.begin(),
                    mPathPayment.path.end());

    // update last balance in the chain
    {
        if (curB.type() == CURRENCY_TYPE_NATIVE)
        {
            destination.getAccount().balance += curBReceived;
            destination.storeChange(delta, db);
        }
        else
        {
            TrustFrame destLine;

            if (!TrustFrame::loadTrustLine(destination.getID(), curB, destLine,
                                           db))
            {
                innerResult().code(PATH_PAYMENT_NO_TRUST);
                return false;
            }

            if (!destLine.isAuthorized())
            {
                innerResult().code(PATH_PAYMENT_NOT_AUTHORIZED);
                return false;
            }

            if (!destLine.addBalance(curBReceived))
            {
                innerResult().code(PATH_PAYMENT_LINE_FULL);
                return false;
            }

            destLine.storeChange(delta, db);
        }

        innerResult().success().last =
            SimplePaymentResult(destination.getID(), curB, curBReceived);
    }

    // now, walk the path backwards
    for (int i = (int)fullPath.size() - 1; i >= 0; i--)
    {
        int64_t curASent, actualCurBReceived;
        Currency const& curA = fullPath[i];

        using xdr::operator==;

        if (curA == curB)
        {
            continue;
        }

        OfferExchange oe(delta, ledgerManager);

        // curA -> curB
        OfferExchange::ConvertResult r =
            oe.convertWithOffers(curA, INT64_MAX, curASent, curB, curBReceived,
                                 actualCurBReceived, nullptr);
        switch (r)
        {
        case OfferExchange::eFilterStop:
            assert(false); // no filter -> should not happen
            break;
        case OfferExchange::eOK:
            if (curBReceived == actualCurBReceived)
            {
                break;
            }
        // fall through
        case OfferExchange::ePartial:
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
        innerResult().code(PATH_PAYMENT_OVER_SENDMAX);
        return false;
    }

    if (curB.type() == CURRENCY_TYPE_NATIVE)
    {
        int64_t minBalance = mSourceAccount->getMinimumBalance(ledgerManager);

        if (mSourceAccount->getAccount().balance < (minBalance + curBSent))
        { // they don't have enough to send
            innerResult().code(PATH_PAYMENT_UNDERFUNDED);
            return false;
        }

        mSourceAccount->getAccount().balance -= curBSent;
        mSourceAccount->storeChange(delta, db);
    }
    else
    {
        AccountFrame issuer;
        if (!AccountFrame::loadAccount(curB.alphaNum().issuer, issuer, db))
        {
            throw std::runtime_error("sendCredit Issuer not found");
        }

        TrustFrame sourceLineFrame;
        if (!TrustFrame::loadTrustLine(getSourceID(), curB, sourceLineFrame,
                                       db))
        {
            innerResult().code(PATH_PAYMENT_UNDERFUNDED);
            return false;
        }

        if (!sourceLineFrame.addBalance(-curBSent))
        {
            innerResult().code(PATH_PAYMENT_UNDERFUNDED);
            return false;
        }

        sourceLineFrame.storeChange(delta, db);
    }

    return true;
}

bool
PathPaymentOpFrame::doCheckValid()
{
    if (mPathPayment.destAmount <= 0 || mPathPayment.sendMax <= 0)
    {
        innerResult().code(PATH_PAYMENT_MALFORMED);
        return false;
    }
    if (!isCurrencyValid(mPathPayment.sendCurrency) ||
        !isCurrencyValid(mPathPayment.destCurrency))
    {
        innerResult().code(PATH_PAYMENT_MALFORMED);
        return false;
    }
    auto const& p = mPathPayment.path;
    if (!std::all_of(p.begin(), p.end(), isCurrencyValid))
    {
        innerResult().code(PATH_PAYMENT_MALFORMED);
        return false;
    }
    return true;
}
}
