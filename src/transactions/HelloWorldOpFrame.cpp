// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#ifdef XDR_HELLO_WORLD

#include "transactions/HelloWorldOpFrame.h"
#include "transactions/TransactionFrame.h"

namespace stellar
{
HelloWorldOpFrame::HelloWorldOpFrame(Operation const& op,
                                     TransactionFrame const& parentTx)
    : OperationFrame(op, parentTx)
{
}

bool
HelloWorldOpFrame::doApply(AppConnector& app, AbstractLedgerTxn& ltx,
                           OperationResult& res,
                           OperationMetaBuilder& opMeta) const
{
    innerResult(res).code(HELLO_WORLD_SUCCESS);
    return true;
}

bool
HelloWorldOpFrame::doCheckValid(uint32_t ledgerVersion,
                                OperationResult& res) const
{
    return true;
}

ThresholdLevel
HelloWorldOpFrame::getThresholdLevel() const
{
    return ThresholdLevel::LOW;
}
}

#endif // XDR_HELLO_WORLD
