// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/BumpSequenceOpFrame.h"
#include "crypto/SignerKey.h"
#include "database/Database.h"
#include "main/Application.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"

namespace stellar
{
using xdr::operator==;


BumpSequenceOpFrame::BumpSequenceOpFrame(Operation const& op, OperationResult& res,
                                     TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
    , mBumpSequence(mOperation.body.bumpSequenceOp())
{
}

ThresholdLevel
BumpSequenceOpFrame::getThresholdLevel() const
{
    // bumping sequence is low threshold
    return ThresholdLevel::LOW;
}

bool
BumpSequenceOpFrame::doApply(Application& app, LedgerDelta& delta,
                           LedgerManager& ledgerManager)
{
    Database& db = ledgerManager.getDatabase();

    AccountFrame::pointer bumpAccount = AccountFrame::loadAccount(delta, mBumpSequence.bumpAccount, db);

    if (!bumpAccount)
    {
        app.getMetrics()
            .NewMeter({"op-bump-sequence", "failure", "no-account"},
                    "operation")
            .Mark();
        innerResult().code(BUMP_SEQ_NO_ACCOUNT);
        return false;
    }

    SequenceNumber current = bumpAccount->getSeqNum();
    if (mBumpSequence.range && (current < mBumpSequence.range->min || current > mBumpSequence.range->max)) {
        app.getMetrics()
            .NewMeter({"op-bump-sequence", "failure", "out-of-range"},
                    "operation")
            .Mark();
        innerResult().code(BUMP_SEQ_OUT_OF_RANGE);
        return false;
    }

    bumpAccount->setSeqNum(std::max(mBumpSequence.bumpTo, current));
    app.getMetrics()
        .NewMeter({"op-bump-sequence", "success", "apply"}, "operation")
        .Mark();
    innerResult().code(BUMP_SEQ_SUCCESS);
    bumpAccount->storeChange(delta, db);
    return true;
}

bool
BumpSequenceOpFrame::doCheckValid(Application& app)
{
    if (mBumpSequence.bumpAccount == getSourceID()) {
        app.getMetrics()
            .NewMeter({"op-bump-sequence", "failure", "no-self-bump"},
                    "operation")
            .Mark();
        innerResult().code(BUMP_SEQ_NO_SELF_BUMP);
        return false;
    }

    if (mBumpSequence.range) {
        if (mBumpSequence.range->max < mBumpSequence.range->min) {
            app.getMetrics()
                .NewMeter({"op-bump-sequence", "failure", "invalid-range"},
                        "operation")
                .Mark();
            innerResult().code(BUMP_SEQ_INVALID_RANGE);
            return false;
        }

    }

    return true;
}
}
