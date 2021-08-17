// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/LiquidityPoolWithdrawOpFrame.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/TrustLineWrapper.h"
#include "transactions/TransactionUtils.h"

namespace stellar
{

LiquidityPoolWithdrawOpFrame::LiquidityPoolWithdrawOpFrame(
    Operation const& op, OperationResult& res, TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
    , mLiquidityPoolWithdraw(mOperation.body.liquidityPoolWithdrawOp())
{
}

bool
LiquidityPoolWithdrawOpFrame::isVersionSupported(uint32_t protocolVersion) const
{
    return protocolVersion >= 18;
}

bool
LiquidityPoolWithdrawOpFrame::doApply(AbstractLedgerTxn& ltx)
{
    auto tlPool = loadPoolShareTrustLine(
        ltx, getSourceID(), mLiquidityPoolWithdraw.liquidityPoolID);
    if (!tlPool)
    {
        innerResult().code(LIQUIDITY_POOL_WITHDRAW_NO_TRUST);
        return false;
    }

    auto header = ltx.loadHeader();
    if (getAvailableBalance(header, tlPool) < mLiquidityPoolWithdraw.amount)
    {
        innerResult().code(LIQUIDITY_POOL_WITHDRAW_UNDERFUNDED);
        return false;
    }

    // if the pool trustline exists, the pool must exist
    auto poolEntry =
        loadLiquidityPool(ltx, mLiquidityPoolWithdraw.liquidityPoolID);

    // use a lambda so we don't hold a reference to the internals of
    // LiquidityPoolEntry
    auto constantProduct = [&]() -> auto&
    {
        return poolEntry.current().data.liquidityPool().body.constantProduct();
    };

    auto amountA = getPoolWithdrawalAmount(mLiquidityPoolWithdraw.amount,
                                           constantProduct().totalPoolShares,
                                           constantProduct().reserveA);
    if (!tryAddAssetBalance(ltx, header, constantProduct().params.assetA,
                            mLiquidityPoolWithdraw.minAmountA, amountA))
    {
        return false;
    }

    auto amountB = getPoolWithdrawalAmount(mLiquidityPoolWithdraw.amount,
                                           constantProduct().totalPoolShares,
                                           constantProduct().reserveB);
    if (!tryAddAssetBalance(ltx, header, constantProduct().params.assetB,
                            mLiquidityPoolWithdraw.minAmountB, amountB))
    {
        return false;
    }

    if (!addBalance(header, tlPool, -mLiquidityPoolWithdraw.amount))
    {
        throw std::runtime_error("pool withdrawal invalid");
    }
    constantProduct().totalPoolShares -= mLiquidityPoolWithdraw.amount;
    constantProduct().reserveA -= amountA;
    constantProduct().reserveB -= amountB;

    innerResult().code(LIQUIDITY_POOL_WITHDRAW_SUCCESS);
    return true;
}

bool
LiquidityPoolWithdrawOpFrame::doCheckValid(uint32_t ledgerVersion)
{
    if (mLiquidityPoolWithdraw.amount <= 0 ||
        mLiquidityPoolWithdraw.minAmountA < 0 ||
        mLiquidityPoolWithdraw.minAmountB < 0)
    {
        innerResult().code(LIQUIDITY_POOL_WITHDRAW_MALFORMED);
        return false;
    }
    return true;
}

void
LiquidityPoolWithdrawOpFrame::insertLedgerKeysToPrefetch(
    UnorderedSet<LedgerKey>& keys) const
{
    keys.emplace(liquidityPoolKey(mLiquidityPoolWithdraw.liquidityPoolID));
    keys.emplace(poolShareTrustLineKey(getSourceID(),
                                       mLiquidityPoolWithdraw.liquidityPoolID));
}

bool
LiquidityPoolWithdrawOpFrame::tryAddAssetBalance(AbstractLedgerTxn& ltx,
                                                 LedgerTxnHeader const& header,
                                                 Asset const& asset,
                                                 int64_t minAmount,
                                                 int64_t amount)
{
    if (amount < minAmount)
    {
        innerResult().code(LIQUIDITY_POOL_WITHDRAW_UNDER_MINIMUM);
        return false;
    }

    if (asset.type() == ASSET_TYPE_NATIVE)
    {
        auto sourceAccount = loadSourceAccount(ltx, header);
        if (!addBalance(header, sourceAccount, amount))
        {
            innerResult().code(LIQUIDITY_POOL_WITHDRAW_LINE_FULL);
            return false;
        }
    }
    else
    {
        auto trustline = loadTrustLine(ltx, getSourceID(), asset);
        // 1. trustline should exist if the pool exists
        // 2. trustline should be at least authorized to maintain liabiliities
        // here since a deauthorized trustline would get its balance from a
        // liquidity pool pulled
        if (!trustline.addBalance(header, amount))
        {
            innerResult().code(LIQUIDITY_POOL_WITHDRAW_LINE_FULL);
            return false;
        }
    }

    return true;
}

}
