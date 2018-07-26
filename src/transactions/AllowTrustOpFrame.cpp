// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/AllowTrustOpFrame.h"
#include "database/Database.h"
#include "ledger/LedgerDelta.h"
#include "ledger/LedgerManager.h"
#include "ledger/OfferFrame.h"
#include "ledger/TrustFrame.h"
#include "main/Application.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"

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
AllowTrustOpFrame::doApply(Application& app, LedgerDelta& delta,
                           LedgerManager& ledgerManager)
{
    if (ledgerManager.getCurrentLedgerVersion() > 2)
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

    if (!(mSourceAccount->getAccount().flags & AUTH_REQUIRED_FLAG))
    { // this account doesn't require authorization to
        // hold credit
        app.getMetrics()
            .NewMeter({"op-allow-trust", "failure", "not-required"},
                      "operation")
            .Mark();
        innerResult().code(ALLOW_TRUST_TRUST_NOT_REQUIRED);
        return false;
    }

    if (!(mSourceAccount->getAccount().flags & AUTH_REVOCABLE_FLAG) &&
        !mAllowTrust.authorize)
    {
        app.getMetrics()
            .NewMeter({"op-allow-trust", "failure", "cant-revoke"}, "operation")
            .Mark();
        innerResult().code(ALLOW_TRUST_CANT_REVOKE);
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

    Database& db = ledgerManager.getDatabase();
    TrustFrame::pointer trustLine =
        TrustFrame::loadTrustLine(mAllowTrust.trustor, ci, db, &delta);
    if (!trustLine)
    {
        app.getMetrics()
            .NewMeter({"op-allow-trust", "failure", "no-trust-line"},
                      "operation")
            .Mark();
        innerResult().code(ALLOW_TRUST_NO_TRUST_LINE);
        return false;
    }

    bool wasAuthorized = trustLine->isAuthorized();
    bool didRevokeAuth = wasAuthorized && !mAllowTrust.authorize;
    if (ledgerManager.getCurrentLedgerVersion() >= 10 && didRevokeAuth)
    {
        auto trustAcc =
            AccountFrame::loadAccount(delta, mAllowTrust.trustor, db);

        // Delete all offers owned by the trustor that are either buying or
        // selling the asset which had authorization revoked.
        auto offers = OfferFrame::loadOffersByAccountAndAsset(
            mAllowTrust.trustor, ci, db);
        for (auto& offer : offers)
        {
            delta.recordEntry(*offer);

            if (offer->getBuying() == ci)
            {
                offer->releaseLiabilities(trustAcc, trustLine, nullptr, delta,
                                          db, ledgerManager);
            }
            else if (offer->getSelling() == ci)
            {
                offer->releaseLiabilities(trustAcc, nullptr, trustLine, delta,
                                          db, ledgerManager);
            }

            trustAcc->addNumEntries(-1, ledgerManager);
            trustAcc->storeChange(delta, db);
            offer->storeDelete(delta, db);
        }
    }

    trustLine->setAuthorized(mAllowTrust.authorize);
    trustLine->storeChange(delta, db);

    app.getMetrics()
        .NewMeter({"op-allow-trust", "success", "apply"}, "operation")
        .Mark();
    innerResult().code(ALLOW_TRUST_SUCCESS);
    return true;
}

bool
AllowTrustOpFrame::doCheckValid(Application& app)
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
