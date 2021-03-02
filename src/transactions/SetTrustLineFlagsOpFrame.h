#pragma once

// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/OperationFrame.h"

namespace stellar
{
class AbstractLedgerTxn;

class SetTrustLineFlagsOpFrame : public OperationFrame
{
    ThresholdLevel getThresholdLevel() const override;
    SetTrustLineFlagsResult&
    innerResult()
    {
        return mResult.tr().setTrustLineFlagsResult();
    }

    bool isAuthRevocationValid(AbstractLedgerTxn& ltx);

    SetTrustLineFlagsOp const& mSetTrustLineFlags;

  public:
    SetTrustLineFlagsOpFrame(Operation const& op, OperationResult& res,
                             TransactionFrame& parentTx);

    bool isVersionSupported(uint32_t protocolVersion) const override;

    bool doApply(AbstractLedgerTxn& ltx) override;
    bool doCheckValid(uint32_t ledgerVersion) override;
    void
    insertLedgerKeysToPrefetch(UnorderedSet<LedgerKey>& keys) const override;

    static SetTrustLineFlagsResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().setTrustLineFlagsResult().code();
    }
};
}
