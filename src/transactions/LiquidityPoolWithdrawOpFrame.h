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
    innerResult(OperationResult& res) const
    {
        return res.tr().liquidityPoolWithdrawResult();
    }
    LiquidityPoolWithdrawOp const& mLiquidityPoolWithdraw;

    bool tryAddAssetBalance(AbstractLedgerTxn& ltx, OperationResult& res,
                            LedgerTxnHeader const& header, Asset const& asset,
                            int64_t minAmount, int64_t amount) const;

  public:
    LiquidityPoolWithdrawOpFrame(Operation const& op,
                                 TransactionFrame const& parentTx);

    bool isOpSupported(LedgerHeader const& header) const override;

    bool doApply(AppConnector& app, AbstractLedgerTxn& ltx,
                 Hash const& sorobanBasePrngSeed, OperationResult& res,
                 std::shared_ptr<SorobanTxData> sorobanData) const override;
    bool doCheckValid(uint32_t ledgerVersion,
                      OperationResult& res) const override;
    void
    insertLedgerKeysToPrefetch(UnorderedSet<LedgerKey>& keys) const override;

    static LiquidityPoolWithdrawResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().liquidityPoolWithdrawResult().code();
    }
};
}
