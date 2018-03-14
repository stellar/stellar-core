// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/MergeOpFrame.h"
#include "database/Database.h"
#include "ledger/LedgerHeaderReference.h"
#include "ledger/LedgerState.h"
#include "main/Application.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "transactions/TransactionFrame.h"
#include "transactions/TransactionUtils.h"
#include "util/Logging.h"

#include "ledger/AccountReference.h"

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
MergeOpFrame::doApply(Application& app, LedgerState& ls)
{
    auto sourceAccount = loadSourceAccount(ls);
    int64 sourceBalance = sourceAccount.getBalance();

    auto otherAccount =
        stellar::loadAccount(ls, mOperation.body.destination());
    if (!otherAccount)
    {
        app.getMetrics()
            .NewMeter({"op-merge", "failure", "no-account"}, "operation")
            .Mark();
        innerResult().code(ACCOUNT_MERGE_NO_ACCOUNT);
        return false;
    }

    auto header = ls.loadHeader();
    if (getCurrentLedgerVersion(header) > 4 &&
        getCurrentLedgerVersion(header) < 8)
    {
        // in versions < 8, merge account could be called with a stale account
        sourceAccount.forget(ls);
        auto thisAccount = stellar::loadAccount(ls, getSourceID());
        if (!thisAccount)
        {
            app.getMetrics()
                .NewMeter({"op-merge", "failure", "no-account"}, "operation")
                .Mark();
            innerResult().code(ACCOUNT_MERGE_NO_ACCOUNT);
            return false;
        }
        if (getCurrentLedgerVersion(header) > 5)
        {
            sourceBalance = thisAccount.getBalance();
        }
        thisAccount.invalidate();
        sourceAccount = loadSourceAccount(ls, header);
    }

    if (sourceAccount.isImmutableAuth())
    {
        app.getMetrics()
            .NewMeter({"op-merge", "failure", "static-auth"}, "operation")
            .Mark();
        innerResult().code(ACCOUNT_MERGE_IMMUTABLE_SET);
        return false;
    }

    if (sourceAccount.account().numSubEntries !=
        sourceAccount.account().signers.size())
    {
        app.getMetrics()
            .NewMeter({"op-merge", "failure", "has-sub-entries"}, "operation")
            .Mark();
        innerResult().code(ACCOUNT_MERGE_HAS_SUB_ENTRIES);
        return false;
    }

    if (getCurrentLedgerVersion(header) >= 10)
    {
        SequenceNumber maxSeq = getStartingSequenceNumber(header);

        // don't allow the account to be merged if recreating it would cause it
        // to jump backwards
        if (sourceAccount.getSeqNum() >= maxSeq)
        {
            app.getMetrics()
                .NewMeter({"op-merge", "failure", "too-far"}, "operation")
                .Mark();
            innerResult().code(ACCOUNT_MERGE_SEQNUM_TOO_FAR);
            return false;
        }
    }
    header->invalidate();

    // "success" path starts
    if (!otherAccount.addBalance(sourceBalance))
    {
        throw std::runtime_error("merge overflowed destination balance");
    }

    sourceAccount.erase();

    app.getMetrics()
        .NewMeter({"op-merge", "success", "apply"}, "operation")
        .Mark();
    innerResult().code(ACCOUNT_MERGE_SUCCESS);
    innerResult().sourceAccountBalance() = sourceBalance;
    return true;
}

bool
MergeOpFrame::doCheckValid(Application& app, uint32_t ledgerVersion)
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
