// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "transactions/InflationOpFrame.h"

namespace stellar
{
InflationOpFrame::InflationOpFrame(Operation const& op, OperationResult& res,
                                   TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
{
}
bool
InflationOpFrame::doApply(LedgerDelta& delta, LedgerManagerImpl& ledgerMaster)
{
    // TODO.2
    innerResult().code(Inflation::NOT_TIME);
    return false;
}

bool
InflationOpFrame::doCheckValid(Application& app)
{
    return true;
}
}
