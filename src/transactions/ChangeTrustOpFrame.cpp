// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ChangeTrustOpFrame.h"
#include "database/Database.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerState.h"
#include "ledger/LedgerStateEntry.h"
#include "ledger/LedgerStateHeader.h"
#include "ledger/TrustLineWrapper.h"
#include "main/Application.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "transactions/TransactionUtils.h"

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
ChangeTrustOpFrame::doApply(Application& app, AbstractLedgerState& ls)
{
    auto header = ls.loadHeader();
    auto issuerID = getIssuer(mChangeTrust.line);

    if (header.current().ledgerVersion > 2)
    {
        // Note: No longer checking if issuer exists here, because if
        //     issuerID == getSourceID()
        // and issuer does not exist then this operation would have already
        // failed with opNO_ACCOUNT.
        if (issuerID == getSourceID())
        {
            // since version 3 it is not allowed to use CHANGE_TRUST on self
            app.getMetrics()
                .NewMeter({"op-change-trust", "failure", "trust-self"},
                          "operation")
                .Mark();
            innerResult().code(CHANGE_TRUST_SELF_NOT_ALLOWED);
            return false;
        }
    }
    else if (issuerID == getSourceID())
    {
        if (mChangeTrust.limit < INT64_MAX)
        {
            app.getMetrics()
                .NewMeter({"op-change-trust", "failure", "invalid-limit"},
                          "operation")
                .Mark();
            innerResult().code(CHANGE_TRUST_INVALID_LIMIT);
            return false;
        }
        else if (!stellar::loadAccountWithoutRecord(ls, issuerID))
        {
            app.getMetrics()
                .NewMeter({"op-change-trust", "failure", "no-issuer"},
                          "operation")
                .Mark();
            innerResult().code(CHANGE_TRUST_NO_ISSUER);
            return false;
        }
        return true;
    }

    LedgerKey key(TRUSTLINE);
    key.trustLine().accountID = getSourceID();
    key.trustLine().asset = mChangeTrust.line;

    auto trustLine = ls.load(key);
    if (trustLine)
    { // we are modifying an old trustline
        if (mChangeTrust.limit < getMinimumLimit(header, trustLine))
        {
            // Can't drop the limit below the balance you are holding with them
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
            trustLine.erase();
            auto sourceAccount = loadSourceAccount(ls, header);
            addNumEntries(header, sourceAccount, -1);
        }
        else
        {
            auto issuer = stellar::loadAccountWithoutRecord(ls, issuerID);
            if (!issuer)
            {
                app.getMetrics()
                    .NewMeter({"op-change-trust", "failure", "no-issuer"},
                              "operation")
                    .Mark();
                innerResult().code(CHANGE_TRUST_NO_ISSUER);
                return false;
            }
            trustLine.current().data.trustLine().limit = mChangeTrust.limit;
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
        auto issuer = stellar::loadAccountWithoutRecord(ls, issuerID);
        if (!issuer)
        {
            app.getMetrics()
                .NewMeter({"op-change-trust", "failure", "no-issuer"},
                          "operation")
                .Mark();
            innerResult().code(CHANGE_TRUST_NO_ISSUER);
            return false;
        }

        auto sourceAccount = loadSourceAccount(ls, header);
        if (!addNumEntries(header, sourceAccount, 1))
        {
            app.getMetrics()
                .NewMeter({"op-change-trust", "failure", "low-reserve"},
                          "operation")
                .Mark();
            innerResult().code(CHANGE_TRUST_LOW_RESERVE);
            return false;
        }

        LedgerEntry trustLineEntry;
        trustLineEntry.data.type(TRUSTLINE);
        auto& tl = trustLineEntry.data.trustLine();
        tl.accountID = getSourceID();
        tl.asset = mChangeTrust.line;
        tl.limit = mChangeTrust.limit;
        tl.balance = 0;
        if (!isAuthRequired(issuer))
        {
            tl.flags = AUTHORIZED_FLAG;
        }
        ls.create(trustLineEntry);

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
