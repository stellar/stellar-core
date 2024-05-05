#pragma once

// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/OperationFrame.h"

namespace stellar
{

class AbstractLedgerTxn;

class LiquidityPoolWithdrawOpFrame : public OperationFrame
{
    LiquidityPoolWithdrawResult&
    innerResult()
    {
        return mResult.tr().liquidityPoolWithdrawResult();
    }
    LiquidityPoolWithdrawOp const& mLiquidityPoolWithdraw;

    bool tryAddAssetBalance(AbstractLedgerTxn& ltx,
                            TransactionResultPayload& resPayload,
                            LedgerTxnHeader const& header, Asset const& asset,
                            int64_t minAmount, int64_t amount);

  public:
    LiquidityPoolWithdrawOpFrame(Operation const& op, OperationResult& res,
                                 TransactionFrame& parentTx);

    bool isOpSupported(LedgerHeader const& header) const override;

    bool doApply(AbstractLedgerTxn& ltx,
                 TransactionResultPayload& resPayload) override;
    bool doCheckValid(uint32_t ledgerVersion) override;
    void
    insertLedgerKeysToPrefetch(UnorderedSet<LedgerKey>& keys) const override;

    static LiquidityPoolWithdrawResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().liquidityPoolWithdrawResult().code();
    }
};
}
