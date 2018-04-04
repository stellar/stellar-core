// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ChangeTrustOpFrame.h"
#include "database/Database.h"
#include "ledger/LedgerHeaderReference.h"
#include "ledger/LedgerState.h"
#include "main/Application.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "transactions/TransactionUtils.h"

#include "ledger/AccountReference.h"
#include "ledger/TrustLineReference.h"

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
ChangeTrustOpFrame::doApply(Application& app, LedgerState& ls)
{
    auto issuer = stellar::loadAccount(ls, getIssuer(mChangeTrust.line));
    auto header = ls.loadHeader();
    if (getCurrentLedgerVersion(header) > 2)
    {
        if (issuer && (issuer.getID() == getSourceID()))
        { // since version 3 it is
            // not allowed to use
            // CHANGE_TRUST on self
            app.getMetrics()
                .NewMeter({"op-change-trust", "failure", "trust-self"},
                          "operation")
                .Mark();
            innerResult().code(CHANGE_TRUST_SELF_NOT_ALLOWED);
            return false;
        }
    }
    header->invalidate();

    auto trustLine =
        loadTrustLine(ls, getSourceID(), mChangeTrust.line);
    if (trustLine)
    { // we are modifying an old trustline
        if (mChangeTrust.limit < trustLine->getBalance())
        { // Can't drop the limit
            // below the balance you
            // are holding with them
            app.getMetrics()
                .NewMeter({"op-change-trust", "failure", "invalid-limit"},
                          "operation")
                .Mark();
            innerResult().code(CHANGE_TRUST_INVALID_LIMIT);
            return false;
        }

        if (mChangeTrust.limit == 0)
        {
            // line gets deleted
            trustLine->erase();
            auto sourceAccount = loadSourceAccount(ls);
            auto header = ls.loadHeader();
            sourceAccount.addNumEntries(header, -1);
            header->invalidate();
            if (issuer)
            {
                issuer.forget(ls);
            }
        }
        else
        {
            if (!issuer)
            {
                app.getMetrics()
                    .NewMeter({"op-change-trust", "failure", "no-issuer"},
                              "operation")
                    .Mark();
                innerResult().code(CHANGE_TRUST_NO_ISSUER);
                return false;
            }
            trustLine->setLimit(mChangeTrust.limit);
            issuer.forget(ls);
        }

        app.getMetrics()
            .NewMeter({"op-change-trust", "success", "apply"}, "operation")
            .Mark();
        innerResult().code(CHANGE_TRUST_SUCCESS);
        return true;
    }
    else
    { // new trust line
        if (mChangeTrust.limit == 0)
        {
            app.getMetrics()
                .NewMeter({"op-change-trust", "failure", "invalid-limit"},
                          "operation")
                .Mark();
            innerResult().code(CHANGE_TRUST_INVALID_LIMIT);
            return false;
        }
        if (!issuer)
        {
            app.getMetrics()
                .NewMeter({"op-change-trust", "failure", "no-issuer"},
                          "operation")
                .Mark();
            innerResult().code(CHANGE_TRUST_NO_ISSUER);
            return false;
        }

        auto sourceAccount = loadSourceAccount(ls);
        auto header = ls.loadHeader();
        if (!sourceAccount.addNumEntries(header, 1))
        {
            app.getMetrics()
                .NewMeter({"op-change-trust", "failure", "low-reserve"},
                          "operation")
                .Mark();
            innerResult().code(CHANGE_TRUST_LOW_RESERVE);
            return false;
        }
        header->invalidate();

        // Moved this after LOW_RESERVE check to reproduce old behavior
        LedgerEntry newTrustLine;
        newTrustLine.data.type(TRUSTLINE);
        newTrustLine.data.trustLine().accountID = getSourceID();
        newTrustLine.data.trustLine().asset = mChangeTrust.line;
        newTrustLine.data.trustLine().limit = mChangeTrust.limit;
        newTrustLine.data.trustLine().balance = 0;
        trustLine = std::make_shared<ExplicitTrustLineReference>(ls.create(newTrustLine));
        trustLine->setAuthorized(!issuer.isAuthRequired());
        issuer.forget(ls);

        app.getMetrics()
            .NewMeter({"op-change-trust", "success", "apply"}, "operation")
            .Mark();
        innerResult().code(CHANGE_TRUST_SUCCESS);
        return true;
    }
}

bool
ChangeTrustOpFrame::doCheckValid(Application& app, uint32_t ledgerVersion)
{
    if (mChangeTrust.limit < 0)
    {
        app.getMetrics()
            .NewMeter(
                {"op-change-trust", "invalid", "malformed-negative-limit"},
                "operation")
            .Mark();
        innerResult().code(CHANGE_TRUST_MALFORMED);
        return false;
    }
    if (!isAssetValid(mChangeTrust.line))
    {
        app.getMetrics()
            .NewMeter({"op-change-trust", "invalid", "malformed-invalid-asset"},
                      "operation")
            .Mark();
        innerResult().code(CHANGE_TRUST_MALFORMED);
        return false;
    }
    if (ledgerVersion > 9)
    {
        if (mChangeTrust.line.type() == ASSET_TYPE_NATIVE)
        {
            app.getMetrics()
                .NewMeter(
                    {"op-change-trust", "invalid", "malformed-native-asset"},
                    "operation")
                .Mark();
            innerResult().code(CHANGE_TRUST_MALFORMED);
            return false;
        }
    }
    return true;
}
}
