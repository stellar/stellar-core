#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/OperationFrame.h"

namespace stellar
{
class AllowTrustOpFrame : public OperationFrame
{
    ThresholdLevel getThresholdLevel() const override;
    AllowTrustResult&
    innerResult() const
    {
        return getResult().tr().allowTrustResult();
    }

    AllowTrustOp const& mAllowTrust;

  public:
    AllowTrustOpFrame(Operation const& op, OperationResult& res,
                      TransactionFrame& parentTx);

    bool doApply(Application& app, LedgerDelta& ledgerDelta,
                 LedgerManager& ledgerManager) override;
    bool doCheckValid(Application& app) override;

    static AllowTrustResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().allowTrustResult().code();
    }
};
}
