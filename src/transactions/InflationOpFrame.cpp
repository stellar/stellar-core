// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/InflationOpFrame.h"

namespace stellar
{
InflationOpFrame::InflationOpFrame(Operation const& op, OperationResult& res,
                                   TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
{
}
bool
InflationOpFrame::doApply(LedgerDelta& delta, LedgerManager& ledgerManager)
{
    // TODO.2
    innerResult().code(INFLATION_NOT_TIME);
    return false;
}

bool
InflationOpFrame::doCheckValid(Application& app)
{
    return true;
}
}
