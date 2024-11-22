#pragma once

// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/OperationFrame.h"

namespace stellar
{

class AbstractLedgerTxn;

typedef LiquidityPoolEntry::_body_t::_constantProduct_t
    LiquidityPoolConstantProduct;

class LiquidityPoolDepositOpFrame : public OperationFrame
{
    LiquidityPoolDepositResult&
    innerResult(OperationResult& res) const
    {
        return res.tr().liquidityPoolDepositResult();
    }

    bool depositIntoEmptyPool(int64_t& amountA, int64_t& amountB,
                              int64_t& amountPoolShares, int64_t availableA,
                              int64_t availableB,
                              int64_t availableLimitPoolShares,
                              OperationResult& res) const;

    bool depositIntoNonEmptyPool(int64_t& amountA, int64_t& amountB,
                                 int64_t& amountPoolShares, int64_t availableA,
                                 int64_t availableB,
                                 int64_t availableLimitPoolShares,
                                 LiquidityPoolConstantProduct const& cp,
                                 OperationResult& res) const;

    LiquidityPoolDepositOp const& mLiquidityPoolDeposit;

  public:
    LiquidityPoolDepositOpFrame(Operation const& op,
                                TransactionFrame const& parentTx);

    bool isOpSupported(LedgerHeader const& header) const override;

    bool doApply(AppConnector& app, AbstractLedgerTxn& ltx,
                 Hash const& sorobanBasePrngSeed, OperationResult& res,
                 std::shared_ptr<SorobanTxData> sorobanData) const override;
    bool doCheckValid(uint32_t ledgerVersion,
                      OperationResult& res) const override;
    void
    insertLedgerKeysToPrefetch(UnorderedSet<LedgerKey>& keys) const override;

    static LiquidityPoolDepositResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().liquidityPoolDepositResult().code();
    }
};
}
