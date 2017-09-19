#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/OperationFrame.h"

namespace stellar
{
class InflationOpFrame : public OperationFrame
{
    ThresholdLevel getThresholdLevel() const override;

  public:
    InflationOpFrame(Operation const& op, TransactionFrame& parentTx);

    OperationResult doApply(Application& app, LedgerDelta& delta,
                 LedgerManager& ledgerManager) override;
    OperationResult doCheckValid(Application& app) override;

    static InflationResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().inflationResult().code();
    }
};
}
