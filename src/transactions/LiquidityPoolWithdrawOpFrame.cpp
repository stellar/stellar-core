// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/LiquidityPoolWithdrawOpFrame.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/TrustLineWrapper.h"
#include "transactions/TransactionUtils.h"
#include "util/ProtocolVersion.h"
#include <Tracy.hpp>

namespace stellar
{

LiquidityPoolWithdrawOpFrame::LiquidityPoolWithdrawOpFrame(
    Operation const& op, TransactionFrame const& parentTx)
    : OperationFrame(op, parentTx)
    , mLiquidityPoolWithdraw(mOperation.body.liquidityPoolWithdrawOp())
{
}

bool
LiquidityPoolWithdrawOpFrame::isOpSupported(LedgerHeader const& header) const
{
    return protocolVersionStartsFrom(header.ledgerVersion,
                                     ProtocolVersion::V_18) &&
           !isPoolWithdrawalDisabled(header);
}

bool
LiquidityPoolWithdrawOpFrame::doApply(
    AppConnector& app, AbstractLedgerTxn& ltx, Hash const& sorobanBasePrngSeed,
    OperationResult& res, std::shared_ptr<SorobanTxData> sorobanData) const
{
    ZoneNamedN(applyZone, "LiquidityPoolWithdrawOpFrame apply", true);

    auto tlPool = loadPoolShareTrustLine(
        ltx, getSourceID(), mLiquidityPoolWithdraw.liquidityPoolID);
    if (!tlPool)
    {
        innerResult(res).code(LIQUIDITY_POOL_WITHDRAW_NO_TRUST);
        return false;
    }

    auto header = ltx.loadHeader();
    if (getAvailableBalance(header, tlPool) < mLiquidityPoolWithdraw.amount)
    {
        innerResult(res).code(LIQUIDITY_POOL_WITHDRAW_UNDERFUNDED);
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
    if (!tryAddAssetBalance(ltx, res, header, constantProduct().params.assetA,
                            mLiquidityPoolWithdraw.minAmountA, amountA))
    {
        return false;
    }

    auto amountB = getPoolWithdrawalAmount(mLiquidityPoolWithdraw.amount,
                                           constantProduct().totalPoolShares,
                                           constantProduct().reserveB);
    if (!tryAddAssetBalance(ltx, res, header, constantProduct().params.assetB,
                            mLiquidityPoolWithdraw.minAmountB, amountB))
    {
        return false;
    }

    if (!addBalance(header, tlPool, -mLiquidityPoolWithdraw.amount))
    {
        throw std::runtime_error("pool withdrawal invalid");
    }

    if (!addBalance(constantProduct().totalPoolShares,
                    -mLiquidityPoolWithdraw.amount))
    {
        throw std::runtime_error("insufficient totalPoolShares");
    }

    if (!addBalance(constantProduct().reserveA, -amountA))
    {
        throw std::runtime_error("insufficient reserveA");
    }

    if (!addBalance(constantProduct().reserveB, -amountB))
    {
        throw std::runtime_error("insufficient reserveB");
    }

    innerResult(res).code(LIQUIDITY_POOL_WITHDRAW_SUCCESS);
    return true;
}

bool
LiquidityPoolWithdrawOpFrame::doCheckValid(uint32_t ledgerVersion,
                                           OperationResult& res) const
{
    if (mLiquidityPoolWithdraw.amount <= 0 ||
        mLiquidityPoolWithdraw.minAmountA < 0 ||
        mLiquidityPoolWithdraw.minAmountB < 0)
    {
        innerResult(res).code(LIQUIDITY_POOL_WITHDRAW_MALFORMED);
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
LiquidityPoolWithdrawOpFrame::tryAddAssetBalance(
    AbstractLedgerTxn& ltx, OperationResult& res, LedgerTxnHeader const& header,
    Asset const& asset, int64_t minAmount, int64_t amount) const
{
    if (amount < minAmount)
    {
        innerResult(res).code(LIQUIDITY_POOL_WITHDRAW_UNDER_MINIMUM);
        return false;
    }

    if (asset.type() == ASSET_TYPE_NATIVE)
    {
        auto sourceAccount = loadSourceAccount(ltx, header);
        if (!addBalance(header, sourceAccount, amount))
        {
            innerResult(res).code(LIQUIDITY_POOL_WITHDRAW_LINE_FULL);
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
            innerResult(res).code(LIQUIDITY_POOL_WITHDRAW_LINE_FULL);
            return false;
        }
    }

    return true;
}

}
