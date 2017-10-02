// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/MergeOpFrame.h"
#include "database/Database.h"
#include "ledger/TrustFrame.h"
#include "main/Application.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "util/Logging.h"

using namespace soci;

namespace stellar
{
using xdr::operator==;

MergeOpFrame::MergeOpFrame(Operation const& op, OperationResult& res,
                           TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
{
}

ThresholdLevel
MergeOpFrame::getThresholdLevel() const
{
    return ThresholdLevel::HIGH;
}

// make sure the deleted Account hasn't issued credit
// make sure we aren't holding any credit
// make sure the we delete all the offers
// make sure the we delete all the trustlines
// move the XLM to the new account
bool
MergeOpFrame::doApply(Application& app, LedgerDelta& delta,
                      LedgerManager& ledgerManager)
{
    AccountFrame::pointer otherAccount;
    Database& db = ledgerManager.getDatabase();
    auto const& sourceAccount = mSourceAccount->getAccount();
    int64 sourceBalance = sourceAccount.balance;

    otherAccount =
        AccountFrame::loadAccount(delta, mOperation.body.destination(), db);

    if (!otherAccount)
    {
        app.getMetrics()
            .NewMeter({"op-merge", "failure", "no-account"}, "operation")
            .Mark();
        innerResult().code(ACCOUNT_MERGE_NO_ACCOUNT);
        return false;
    }

    if (ledgerManager.getCurrentLedgerVersion() > 4 &&
        ledgerManager.getCurrentLedgerVersion() < 8)
    {
        // in versions < 8, merge account could be called with a stale account
        AccountFrame::pointer thisAccount =
            AccountFrame::loadAccount(delta, mSourceAccount->getID(), db);
        if (!thisAccount)
        {
            app.getMetrics()
                .NewMeter({"op-merge", "failure", "no-account"}, "operation")
                .Mark();
            innerResult().code(ACCOUNT_MERGE_NO_ACCOUNT);
            return false;
        }
        if (ledgerManager.getCurrentLedgerVersion() > 5)
        {
            sourceBalance = thisAccount->getBalance();
        }
    }

    if (mSourceAccount->isImmutableAuth())
    {
        app.getMetrics()
            .NewMeter({"op-merge", "failure", "static-auth"}, "operation")
            .Mark();
        innerResult().code(ACCOUNT_MERGE_IMMUTABLE_SET);
        return false;
    }

    if (sourceAccount.numSubEntries != sourceAccount.signers.size())
    {
        app.getMetrics()
            .NewMeter({"op-merge", "failure", "has-sub-entries"}, "operation")
            .Mark();
        innerResult().code(ACCOUNT_MERGE_HAS_SUB_ENTRIES);
        return false;
    }

    if (!otherAccount->addBalance(sourceBalance))
    {
        throw std::runtime_error("merge overflowed destination balance");
    }

    otherAccount->storeChange(delta, db);

    if (ledgerManager.getCurrentLedgerVersion() < 8)
    {
        // we have to compensate for buggy behavior in version < 8
        // to avoid tripping invariants
        AccountFrame::pointer thisAccount =
            AccountFrame::loadAccount(delta, mSourceAccount->getID(), db);
        if (!thisAccount)
        {
            // ignore double delete
        }
        else
        {
            mSourceAccount->storeDelete(delta, db);
        }
    }
    else
    {
        mSourceAccount->storeDelete(delta, db);
    }

    app.getMetrics()
        .NewMeter({"op-merge", "success", "apply"}, "operation")
        .Mark();
    innerResult().code(ACCOUNT_MERGE_SUCCESS);
    innerResult().sourceAccountBalance() = sourceBalance;
    return true;
}

bool
MergeOpFrame::doCheckValid(Application& app)
{
    // makes sure not merging into self
    if (getSourceID() == mOperation.body.destination())
    {
        app.getMetrics()
            .NewMeter({"op-merge", "invalid", "malformed-self-merge"},
                      "operation")
            .Mark();
        innerResult().code(ACCOUNT_MERGE_MALFORMED);
        return false;
    }
    return true;
}
}
