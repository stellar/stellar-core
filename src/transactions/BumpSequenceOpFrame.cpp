// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/BumpSequenceOpFrame.h"
#include "crypto/SignerKey.h"
#include "database/Database.h"
#include "main/Application.h"
#include "transactions/TransactionFrame.h"
#include "transactions/TransactionUtils.h"
#include "util/ProtocolVersion.h"
#include "util/XDROperators.h"
#include <Tracy.hpp>

namespace stellar
{
BumpSequenceOpFrame::BumpSequenceOpFrame(Operation const& op,
                                         TransactionFrame const& parentTx)
    : OperationFrame(op, parentTx)
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
BumpSequenceOpFrame::isOpSupported(LedgerHeader const& header) const
{
    return protocolVersionStartsFrom(header.ledgerVersion,
                                     ProtocolVersion::V_10);
}

bool
BumpSequenceOpFrame::doApply(AppConnector& app, AbstractLedgerTxn& ltx,
                             Hash const& sorobanBasePrngSeed,
                             OperationResult& res,
                             std::shared_ptr<SorobanTxData> sorobanData) const
{
    ZoneNamedN(applyZone, "BumpSequenceOp apply", true);
    LedgerTxn ltxInner(ltx);
    auto header = ltxInner.loadHeader();
    auto sourceAccountEntry = loadSourceAccount(ltxInner, header);
    maybeUpdateAccountOnLedgerSeqUpdate(header, sourceAccountEntry);

    auto& sourceAccount = sourceAccountEntry.current().data.account();
    SequenceNumber current = sourceAccount.seqNum;

    // Apply the bump (bump succeeds silently if bumpTo <= current)
    if (mBumpSequenceOp.bumpTo > current)
    {
        sourceAccount.seqNum = mBumpSequenceOp.bumpTo;
        ltxInner.commit();
    }
    else if (protocolVersionStartsFrom(header.current().ledgerVersion,
                                       ProtocolVersion::V_19))
    {
        // we need to commit the changes from
        // maybeUpdateAccountOnLedgerSeqUpdate
        ltxInner.commit();
    }

    // Return successful results
    innerResult(res).code(BUMP_SEQUENCE_SUCCESS);
    return true;
}

bool
BumpSequenceOpFrame::doCheckValid(uint32_t ledgerVersion,
                                  OperationResult& res) const
{
    if (mBumpSequenceOp.bumpTo < 0)
    {
        innerResult(res).code(BUMP_SEQUENCE_BAD_SEQ);
        return false;
    }
    return true;
}
}
