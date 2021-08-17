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
    innerResult()
    {
        return mResult.tr().liquidityPoolDepositResult();
    }

    bool depositIntoEmptyPool(int64_t& amountA, int64_t& amountB,
                              int64_t& amountPoolShares, int64_t availableA,
                              int64_t availableB,
                              int64_t availableLimitPoolShares);

    bool depositIntoNonEmptyPool(int64_t& amountA, int64_t& amountB,
                                 int64_t& amountPoolShares, int64_t availableA,
                                 int64_t availableB,
                                 int64_t availableLimitPoolShares,
                                 LiquidityPoolConstantProduct const& cp);

    LiquidityPoolDepositOp const& mLiquidityPoolDeposit;

  public:
    LiquidityPoolDepositOpFrame(Operation const& op, OperationResult& res,
                                TransactionFrame& parentTx);

    bool isVersionSupported(uint32_t protocolVersion) const override;

    bool doApply(AbstractLedgerTxn& ltx) override;
    bool doCheckValid(uint32_t ledgerVersion) override;
    void
    insertLedgerKeysToPrefetch(UnorderedSet<LedgerKey>& keys) const override;

    static LiquidityPoolDepositResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().liquidityPoolDepositResult().code();
    }
};
}
