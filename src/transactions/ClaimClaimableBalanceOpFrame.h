#pragma once

// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/OperationFrame.h"

namespace stellar
{

class AbstractLedgerTxn;

class ClaimClaimableBalanceOpFrame : public OperationFrame
{
    ThresholdLevel getThresholdLevel() const override;
    ClaimClaimableBalanceResult&
    innerResult()
    {
        return mResult.tr().claimClaimableBalanceResult();
    }

    ClaimClaimableBalanceOp const& mClaimClaimableBalance;

  public:
    ClaimClaimableBalanceOpFrame(Operation const& op, OperationResult& res,
                                 TransactionFrame& parentTx);

    bool isOpSupported(LedgerHeader const& header) const override;

    bool doApply(AbstractLedgerTxn& ltx,
                 TransactionResultPayload& resPayload) override;
    bool doCheckValid(uint32_t ledgerVersion) override;
    void
    insertLedgerKeysToPrefetch(UnorderedSet<LedgerKey>& keys) const override;

    static ClaimClaimableBalanceResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().claimClaimableBalanceResult().code();
    }
};
}
