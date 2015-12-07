// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ChangeTrustOpFrame.h"
#include "ledger/TrustFrame.h"
#include "ledger/LedgerManager.h"
#include "database/Database.h"

#include "medida/meter.h"
#include "medida/metrics_registry.h"

namespace stellar
{

ChangeTrustOpFrame::ChangeTrustOpFrame(Operation const& op,
                                       OperationResult& res,
                                       TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
    , mChangeTrust(mOperation.body.changeTrustOp())
{
}
bool
ChangeTrustOpFrame::doApply(medida::MetricsRegistry& metrics,
                            LedgerDelta& delta, LedgerManager& ledgerManager)
{
    Database& db = ledgerManager.getDatabase();

    auto tlI = TrustFrame::loadTrustLineIssuer(getSourceID(), mChangeTrust.line,
                                               db, delta);

    auto& trustLine = tlI.first;
    auto& issuer = tlI.second;

    if (trustLine)
    { // we are modifying an old trustline

        if (mChangeTrust.limit < trustLine->getBalance())
        { // Can't drop the limit below the balance you are holding with them
            metrics.NewMeter({"op-change-trust", "failure", "invalid-limit"},
                             "operation").Mark();
            innerResult().code(CHANGE_TRUST_INVALID_LIMIT);
            return false;
        }

        if (mChangeTrust.limit == 0)
        {
            // line gets deleted
            trustLine->storeDelete(delta, db);
            mSourceAccount->addNumEntries(-1, ledgerManager);
            mSourceAccount->storeChange(delta, db);
        }
        else
        {
            if (!issuer)
            {
                metrics.NewMeter({"op-change-trust", "failure", "no-issuer"},
                                 "operation").Mark();
                innerResult().code(CHANGE_TRUST_NO_ISSUER);
                return false;
            }
            trustLine->getTrustLine().limit = mChangeTrust.limit;
            trustLine->storeChange(delta, db);
        }
        metrics.NewMeter({"op-change-trust", "success", "apply"}, "operation")
            .Mark();
        innerResult().code(CHANGE_TRUST_SUCCESS);
        return true;
    }
    else
    { // new trust line
        if (mChangeTrust.limit == 0)
        {
            metrics.NewMeter({"op-change-trust", "failure", "invalid-limit"},
                             "operation").Mark();
            innerResult().code(CHANGE_TRUST_INVALID_LIMIT);
            return false;
        }
        if (!issuer)
        {
            metrics.NewMeter({"op-change-trust", "failure", "no-issuer"},
                             "operation").Mark();
            innerResult().code(CHANGE_TRUST_NO_ISSUER);
            return false;
        }
        trustLine = std::make_shared<TrustFrame>();
        auto& tl = trustLine->getTrustLine();
        tl.accountID = getSourceID();
        tl.asset = mChangeTrust.line;
        tl.limit = mChangeTrust.limit;
        tl.balance = 0;
        trustLine->setAuthorized(!issuer->isAuthRequired());

        if (!mSourceAccount->addNumEntries(1, ledgerManager))
        {
            metrics.NewMeter({"op-change-trust", "failure", "low-reserve"},
                             "operation").Mark();
            innerResult().code(CHANGE_TRUST_LOW_RESERVE);
            return false;
        }

        mSourceAccount->storeChange(delta, db);
        trustLine->storeAdd(delta, db);

        metrics.NewMeter({"op-change-trust", "success", "apply"}, "operation")
            .Mark();
        innerResult().code(CHANGE_TRUST_SUCCESS);
        return true;
    }
}

bool
ChangeTrustOpFrame::doCheckValid(medida::MetricsRegistry& metrics)
{
    if (mChangeTrust.limit < 0)
    {
        metrics.NewMeter(
                    {"op-change-trust", "invalid", "malformed-negative-limit"},
                    "operation").Mark();
        innerResult().code(CHANGE_TRUST_MALFORMED);
        return false;
    }
    if (!isAssetValid(mChangeTrust.line))
    {
        metrics.NewMeter(
                    {"op-change-trust", "invalid", "malformed-invalid-asset"},
                    "operation").Mark();
        innerResult().code(CHANGE_TRUST_MALFORMED);
        return false;
    }
    return true;
}
}
