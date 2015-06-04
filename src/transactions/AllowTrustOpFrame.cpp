// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/AllowTrustOpFrame.h"
#include "ledger/LedgerManager.h"
#include "ledger/TrustFrame.h"
#include "database/Database.h"

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

int32_t
AllowTrustOpFrame::getNeededThreshold() const
{
    return mSourceAccount->getLowThreshold();
}

bool
AllowTrustOpFrame::doApply(medida::MetricsRegistry& metrics,
                           LedgerDelta& delta, LedgerManager& ledgerManager)
{
    if (!(mSourceAccount->getAccount().flags & AUTH_REQUIRED_FLAG))
    { // this account doesn't require authorization to hold credit
        metrics.NewMeter({"op-allow-trust", "failure", "not-required"},
                         "operation").Mark();
        innerResult().code(ALLOW_TRUST_TRUST_NOT_REQUIRED);
        return false;
    }

    if (!(mSourceAccount->getAccount().flags & AUTH_REVOCABLE_FLAG) &&
        !mAllowTrust.authorize)
    {
        metrics.NewMeter({"op-allow-trust", "failure", "cant-revoke"},
                         "operation").Mark();
        innerResult().code(ALLOW_TRUST_CANT_REVOKE);
        return false;
    }

    Currency ci;
    ci.type(CURRENCY_TYPE_ALPHANUM);
    ci.alphaNum().currencyCode = mAllowTrust.currency.currencyCode();
    ci.alphaNum().issuer = getSourceID();

    Database& db = ledgerManager.getDatabase();
    TrustFrame::pointer trustLine;
    trustLine = TrustFrame::loadTrustLine(mAllowTrust.trustor, ci, db);

    if (!trustLine)
    {
        metrics.NewMeter({"op-allow-trust", "failure", "no-trust-line"},
                         "operation").Mark();
        innerResult().code(ALLOW_TRUST_NO_TRUST_LINE);
        return false;
    }

    metrics.NewMeter({"op-allow-trust", "success", "apply"},
                     "operation").Mark();
    innerResult().code(ALLOW_TRUST_SUCCESS);

    trustLine->setAuthorized(mAllowTrust.authorize);

    trustLine->storeChange(delta, db);

    return true;
}

bool
AllowTrustOpFrame::doCheckValid(medida::MetricsRegistry& metrics)
{
    if (mAllowTrust.currency.type() != CURRENCY_TYPE_ALPHANUM)
    {
        metrics.NewMeter({"op-allow-trust", "invalid",
                          "malformed-non-alphanum"},
                         "operation").Mark();
        innerResult().code(ALLOW_TRUST_MALFORMED);
        return false;
    }
    Currency ci;
    ci.type(CURRENCY_TYPE_ALPHANUM);
    ci.alphaNum().currencyCode = mAllowTrust.currency.currencyCode();
    ci.alphaNum().issuer = getSourceID();

    if (!isCurrencyValid(ci))
    {
        metrics.NewMeter({"op-allow-trust", "invalid",
                          "malformed-invalid-currency"},
                         "operation").Mark();
        innerResult().code(ALLOW_TRUST_MALFORMED);
        return false;
    }

    return true;
}
}
