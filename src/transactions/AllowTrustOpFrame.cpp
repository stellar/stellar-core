// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/AllowTrustOpFrame.h"
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
AllowTrustOpFrame::AllowTrustOpFrame(Operation const& op, OperationResult& res,
                                     TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
    , mAllowTrust(mOperation.body.allowTrustOp())
{
}

ThresholdLevel
AllowTrustOpFrame::getThresholdLevel() const
{
    return ThresholdLevel::LOW;
}

bool
AllowTrustOpFrame::doApply(Application& app, LedgerState& ls)
{
    auto header = ls.loadHeader();
    if (getCurrentLedgerVersion(header) > 2)
    {
        if (mAllowTrust.trustor == getSourceID())
        { // since version 3 it is not
            // allowed to use ALLOW_TRUST on
            // self
            app.getMetrics()
                .NewMeter({"op-allow-trust", "failure", "trust-self"},
                          "operation")
                .Mark();
            innerResult().code(ALLOW_TRUST_SELF_NOT_ALLOWED);
            return false;
        }
    }
    header->invalidate();

    auto sourceAccount = loadSourceAccount(ls);
    if (!(sourceAccount.account().flags & AUTH_REQUIRED_FLAG))
    { // this account doesn't require authorization to
        // hold credit
        app.getMetrics()
            .NewMeter({"op-allow-trust", "failure", "not-required"},
                      "operation")
            .Mark();
        innerResult().code(ALLOW_TRUST_TRUST_NOT_REQUIRED);
        return false;
    }

    if (!(sourceAccount.account().flags & AUTH_REVOCABLE_FLAG) &&
        !mAllowTrust.authorize)
    {
        app.getMetrics()
            .NewMeter({"op-allow-trust", "failure", "cant-revoke"}, "operation")
            .Mark();
        innerResult().code(ALLOW_TRUST_CANT_REVOKE);
        return false;
    }
    sourceAccount.forget(ls);

    // Only possible in ledger version 1 and 2
    if (mAllowTrust.trustor == getSourceID())
    {
        app.getMetrics()
            .NewMeter({"op-allow-trust", "success", "apply"}, "operation")
            .Mark();
        innerResult().code(ALLOW_TRUST_SUCCESS);
        return true;
    }

    Asset ci;
    ci.type(mAllowTrust.asset.type());
    if (mAllowTrust.asset.type() == ASSET_TYPE_CREDIT_ALPHANUM4)
    {
        ci.alphaNum4().assetCode = mAllowTrust.asset.assetCode4();
        ci.alphaNum4().issuer = getSourceID();
    }
    else if (mAllowTrust.asset.type() == ASSET_TYPE_CREDIT_ALPHANUM12)
    {
        ci.alphaNum12().assetCode = mAllowTrust.asset.assetCode12();
        ci.alphaNum12().issuer = getSourceID();
    }

    auto trustLine = loadExplicitTrustLine(ls, mAllowTrust.trustor, ci);
    if (!trustLine)
    {
        app.getMetrics()
            .NewMeter({"op-allow-trust", "failure", "no-trust-line"},
                      "operation")
            .Mark();
        innerResult().code(ALLOW_TRUST_NO_TRUST_LINE);
        return false;
    }

    app.getMetrics()
        .NewMeter({"op-allow-trust", "success", "apply"}, "operation")
        .Mark();
    innerResult().code(ALLOW_TRUST_SUCCESS);

    trustLine->setAuthorized(mAllowTrust.authorize);

    return true;
}

bool
AllowTrustOpFrame::doCheckValid(Application& app, uint32_t ledgerVersion)
{
    if (mAllowTrust.asset.type() == ASSET_TYPE_NATIVE)
    {
        app.getMetrics()
            .NewMeter({"op-allow-trust", "invalid", "malformed-non-alphanum"},
                      "operation")
            .Mark();
        innerResult().code(ALLOW_TRUST_MALFORMED);
        return false;
    }
    Asset ci;
    ci.type(mAllowTrust.asset.type());
    if (mAllowTrust.asset.type() == ASSET_TYPE_CREDIT_ALPHANUM4)
    {
        ci.alphaNum4().assetCode = mAllowTrust.asset.assetCode4();
        ci.alphaNum4().issuer = getSourceID();
    }
    else if (mAllowTrust.asset.type() == ASSET_TYPE_CREDIT_ALPHANUM12)
    {
        ci.alphaNum12().assetCode = mAllowTrust.asset.assetCode12();
        ci.alphaNum12().issuer = getSourceID();
    }

    if (!isAssetValid(ci))
    {
        app.getMetrics()
            .NewMeter({"op-allow-trust", "invalid", "malformed-invalid-asset"},
                      "operation")
            .Mark();
        innerResult().code(ALLOW_TRUST_MALFORMED);
        return false;
    }

    return true;
}
}
