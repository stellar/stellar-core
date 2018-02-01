// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/BumpSequenceOpFrame.h"
#include "crypto/SignerKey.h"
#include "database/Database.h"
#include "main/Application.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "transactions/TransactionFrame.h"

namespace stellar
{
using xdr::operator==;

BumpSequenceOpFrame::BumpSequenceOpFrame(Operation const& op,
                                         OperationResult& res,
                                         TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
    , mBumpSequenceOp(mOperation.body.bumpSequenceOp())
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
    SequenceNumber current = mSourceAccount->getSeqNum();

    SequenceNumber maxBump = ledgerManager.getCurrentLedgerHeader().ledgerSeq;
    maxBump = (maxBump << 32) - 1;

    if (mBumpSequenceOp.bumpTo > maxBump)
    {
        app.getMetrics()
            .NewMeter({"op-bump-sequence", "failure", "too-far"}, "operation")
            .Mark();
        innerResult().code(BUMP_SEQUENCE_TOO_FAR);
        return false;
    }

    // Apply the bump (bump succeeds silently if bumpTo < current)
    mSourceAccount->setSeqNum(std::max(mBumpSequenceOp.bumpTo, current));
    mSourceAccount->storeChange(delta, ledgerManager.getDatabase());

    // Return successful results
    innerResult().code(BUMP_SEQUENCE_SUCCESS);
    app.getMetrics()
        .NewMeter({"op-bump-sequence", "success", "apply"}, "operation")
        .Mark();
    return true;
}

bool
BumpSequenceOpFrame::doCheckValid(Application& app)
{
    if (app.getLedgerManager().getCurrentLedgerVersion() <= 9)
    {
        app.getMetrics()
            .NewMeter({"op-bump-sequence", "failure", "not-supported-yet"},
                      "operation")
            .Mark();
        innerResult().code(BUMP_SEQUENCE_NOT_SUPPORTED_YET);
        return false;
    }

    return true;
}
}
