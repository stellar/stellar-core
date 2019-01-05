// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/BumpSequenceOpFrame.h"
#include "crypto/SignerKey.h"
#include "database/Database.h"
#include "main/Application.h"
#include "transactions/TransactionFrame.h"
#include "util/XDROperators.h"

namespace stellar
{
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
BumpSequenceOpFrame::doApply(Application& app, AbstractLedgerTxn& ltx)
{
    LedgerTxn ltxInner(ltx);
    auto header = ltxInner.loadHeader();
    auto sourceAccountEntry = loadSourceAccount(ltxInner, header);
    auto& sourceAccount = sourceAccountEntry.current().data.account();
    SequenceNumber current = sourceAccount.seqNum;

    // Apply the bump (bump succeeds silently if bumpTo <= current)
    if (mBumpSequenceOp.bumpTo > current)
    {
        sourceAccount.seqNum = mBumpSequenceOp.bumpTo;
        ltxInner.commit();
    }

    // Return successful results
    innerResult().code(BUMP_SEQUENCE_SUCCESS);
    return true;
}

bool
BumpSequenceOpFrame::doCheckValid(Application& app, uint32_t ledgerVersion)
{
    if (mBumpSequenceOp.bumpTo < 0)
    {
        innerResult().code(BUMP_SEQUENCE_BAD_SEQ);
        return false;
    }
    return true;
}
}
