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
BumpSequenceOpFrame::isVersionSupported(uint32_t protocolVersion) const
{
    return protocolVersion >= 10;
}

bool
BumpSequenceOpFrame::doApply(Application& app, LedgerDelta& delta,
                             LedgerManager& ledgerManager)
{
    SequenceNumber current = mSourceAccount->getSeqNum();

    // Apply the bump (bump succeeds silently if bumpTo <= current)
    if (mBumpSequenceOp.bumpTo > current)
    {
        mSourceAccount->setSeqNum(mBumpSequenceOp.bumpTo);
        mSourceAccount->storeChange(delta, ledgerManager.getDatabase());
    }

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
    if (mBumpSequenceOp.bumpTo < 0)
    {
        app.getMetrics()
            .NewMeter({"op-bump-sequence", "invalid", "bad-seq"}, "operation")
            .Mark();
        innerResult().code(BUMP_SEQUENCE_BAD_SEQ);
        return false;
    }
    return true;
}
}
