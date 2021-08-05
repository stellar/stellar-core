// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/LiquidityPoolDepositOpFrame.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/TrustLineWrapper.h"
#include "transactions/TransactionUtils.h"

namespace stellar
{

LiquidityPoolDepositOpFrame::LiquidityPoolDepositOpFrame(
    Operation const& op, OperationResult& res, TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
    , mLiquidityPoolDeposit(mOperation.body.liquidityPoolDepositOp())
{
}

bool
LiquidityPoolDepositOpFrame::isVersionSupported(uint32_t protocolVersion) const
{
    return protocolVersion >= 18;
}

static bool
isBadPrice(int64_t amountA, int64_t amountB, Price const& minPrice,
           Price const& maxPrice)
{
    // a * d < b * n is equivalent to a/b < n/d but avoids rounding.
    if (bigMultiply(amountA, minPrice.d) < bigMultiply(amountB, minPrice.n) ||
        bigMultiply(amountA, maxPrice.d) > bigMultiply(amountB, maxPrice.n))
    {
        return true;
    }
    return false;
}

bool
LiquidityPoolDepositOpFrame::depositIntoEmptyPool(
    int64_t& amountA, int64_t& amountB, int64_t& amountPoolShares,
    int64_t availableA, int64_t availableB, int64_t availableLimitPoolShares)
{
    amountA = mLiquidityPoolDeposit.maxAmountA;
    amountB = mLiquidityPoolDeposit.maxAmountB;

    if (availableA < amountA || availableB < amountB)
    {
        innerResult().code(LIQUIDITY_POOL_DEPOSIT_UNDERFUNDED);
        return false;
    }

    if (isBadPrice(amountA, amountB, mLiquidityPoolDeposit.minPrice,
                   mLiquidityPoolDeposit.maxPrice))
    {
        innerResult().code(LIQUIDITY_POOL_DEPOSIT_BAD_PRICE);
        return false;
    }

    amountPoolShares = bigSquareRoot(amountA, amountB);
    if (availableLimitPoolShares < amountPoolShares)
    {
        innerResult().code(LIQUIDITY_POOL_DEPOSIT_LINE_FULL);
        return false;
    }

    return true;
}

static bool
minAmongValid(int64_t& res, int64_t x, bool xValid, int64_t y, bool yValid)
{
    if (xValid && yValid)
    {
        res = std::min(x, y);
    }
    else if (xValid)
    {
        res = x;
    }
    else if (yValid)
    {
        res = y;
    }
    else
    {
        return false;
    }
    return true;
}

bool
LiquidityPoolDepositOpFrame::depositIntoNonEmptyPool(
    int64_t& amountA, int64_t& amountB, int64_t& amountPoolShares,
    int64_t availableA, int64_t availableB, int64_t availableLimitPoolShares,
    LiquidityPoolConstantProduct const& cp)
{
    int64_t sharesA = 0;
    int64_t sharesB = 0;
    {
        bool resA = bigDivide(sharesA, cp.totalPoolShares,
                              mLiquidityPoolDeposit.maxAmountA, cp.reserveA,
                              ROUND_DOWN);
        bool resB = bigDivide(sharesB, cp.totalPoolShares,
                              mLiquidityPoolDeposit.maxAmountB, cp.reserveB,
                              ROUND_DOWN);
        if (!minAmongValid(amountPoolShares, sharesA, resA, sharesB, resB))
        {
            // sharesA > INT64_MAX and sharesB > INT64_MAX so we always fail
            innerResult().code(LIQUIDITY_POOL_DEPOSIT_POOL_FULL);
            return false;
        }
    }

    {
        bool resA = bigDivide(amountA, amountPoolShares, cp.reserveA,
                              cp.totalPoolShares, ROUND_UP);
        bool resB = bigDivide(amountB, amountPoolShares, cp.reserveB,
                              cp.totalPoolShares, ROUND_UP);
        if (!(resA && resB))
        {
            innerResult().code(LIQUIDITY_POOL_DEPOSIT_UNDERFUNDED);
            return false;
        }
    }

    if (availableA < amountA || availableB < amountB)
    {
        innerResult().code(LIQUIDITY_POOL_DEPOSIT_UNDERFUNDED);
        return false;
    }

    if (isBadPrice(amountA, amountB, mLiquidityPoolDeposit.minPrice,
                   mLiquidityPoolDeposit.maxPrice))
    {
        innerResult().code(LIQUIDITY_POOL_DEPOSIT_BAD_PRICE);
        return false;
    }

    if (availableLimitPoolShares < amountPoolShares)
    {
        innerResult().code(LIQUIDITY_POOL_DEPOSIT_LINE_FULL);
        return false;
    }

    return true;
}

static void
updateBalance(LedgerTxnHeader& header, TrustLineWrapper& tl,
              LedgerTxnEntry& acc, int64_t delta)
{
    if (tl)
    {
        if (!tl.addBalance(header, delta))
        {
            throw std::runtime_error("insufficient balance");
        }
    }
    else
    {
        if (!stellar::addBalance(header, acc, delta))
        {
            throw std::runtime_error("insufficient balance");
        }
    }
}

bool
LiquidityPoolDepositOpFrame::doApply(AbstractLedgerTxn& ltx)
{
    // Don't need TrustLineWrapper here because pool share trust lines cannot be
    // issuer trust lines.
    auto tlPool = loadPoolShareTrustLine(ltx, getSourceID(),
                                         mLiquidityPoolDeposit.liquidityPoolID);
    if (!tlPool)
    {
        innerResult().code(LIQUIDITY_POOL_DEPOSIT_NO_TRUST);
        return false;
    }

    // lp must exist if tlPool exists
    auto lp = loadLiquidityPool(ltx, mLiquidityPoolDeposit.liquidityPoolID);
    auto cp = [&lp]() -> LiquidityPoolConstantProduct& {
        return lp.current().data.liquidityPool().body.constantProduct();
    };
    auto cpp = [&cp]() -> LiquidityPoolConstantProductParameters& {
        return cp().params;
    };
    if (cpp().assetA == cpp().assetB)
    {
        // This should never happen, but let's handle it explicitly
        throw std::runtime_error("Depositing to invalid liquidity pool");
    }

    auto tlA = loadTrustLineIfNotNative(ltx, getSourceID(), cpp().assetA);
    auto tlB = loadTrustLineIfNotNative(ltx, getSourceID(), cpp().assetB);
    if ((cpp().assetA.type() != ASSET_TYPE_NATIVE && !tlA) ||
        (cpp().assetB.type() != ASSET_TYPE_NATIVE && !tlB))
    {
        throw std::runtime_error("Invalid ledger state");
    }
    if ((tlA && !tlA.isAuthorized()) || (tlB && !tlB.isAuthorized()))
    {
        innerResult().code(LIQUIDITY_POOL_DEPOSIT_NOT_AUTHORIZED);
        return false;
    }

    // If one of the assets is native, we'll also need the source account
    LedgerTxnEntry source;
    if (cpp().assetA.type() == ASSET_TYPE_NATIVE ||
        cpp().assetB.type() == ASSET_TYPE_NATIVE)
    {
        // No need to check if it exists, the source account must exist at this
        // point
        source = loadAccount(ltx, getSourceID());
    }

    auto header = ltx.loadHeader();

    int64_t amountA = 0;
    int64_t amountB = 0;
    int64_t amountPoolShares = 0;
    int64_t availableA = tlA ? tlA.getAvailableBalance(header)
                             : getAvailableBalance(header, source);
    int64_t availableB = tlB ? tlB.getAvailableBalance(header)
                             : getAvailableBalance(header, source);
    int64_t availableLimitPoolShares = getMaxAmountReceive(header, tlPool);
    if (cp().totalPoolShares != 0)
    {
        if (!depositIntoNonEmptyPool(amountA, amountB, amountPoolShares,
                                     availableA, availableB,
                                     availableLimitPoolShares, cp()))
        {
            return false;
        }
    }
    else // cp.totalPoolShares == 0
    {
        if (!depositIntoEmptyPool(amountA, amountB, amountPoolShares,
                                  availableA, availableB,
                                  availableLimitPoolShares))
        {
            return false;
        }
    }

    if (INT64_MAX - amountA < cp().reserveA ||
        INT64_MAX - amountB < cp().reserveB ||
        INT64_MAX - amountPoolShares < cp().totalPoolShares)
    {
        innerResult().code(LIQUIDITY_POOL_DEPOSIT_POOL_FULL);
        return false;
    }

    // At most one of these can use source because at most one of tlA and tlB
    // can be null
    updateBalance(header, tlA, source, -amountA);
    if (!stellar::addBalance(cp().reserveA, amountA, INT64_MAX))
    {
        throw std::runtime_error("insufficient liquidity pool limit");
    }
    updateBalance(header, tlB, source, -amountB);
    if (!stellar::addBalance(cp().reserveB, amountB, INT64_MAX))
    {
        throw std::runtime_error("insufficient liquidity pool limit");
    }

    if (!stellar::addBalance(header, tlPool, amountPoolShares))
    {
        throw std::runtime_error("insufficient pool share limit");
    }
    if (!stellar::addBalance(cp().totalPoolShares, amountPoolShares, INT64_MAX))
    {
        throw std::runtime_error("insufficient liquidity pool limit");
    }

    innerResult().code(LIQUIDITY_POOL_DEPOSIT_SUCCESS);
    return true;
}

bool
LiquidityPoolDepositOpFrame::doCheckValid(uint32_t ledgerVersion)
{
    if (mLiquidityPoolDeposit.maxAmountA <= 0 ||
        mLiquidityPoolDeposit.maxAmountB <= 0)
    {
        innerResult().code(LIQUIDITY_POOL_DEPOSIT_MALFORMED);
        return false;
    }

    if (mLiquidityPoolDeposit.minPrice.n <= 0 ||
        mLiquidityPoolDeposit.minPrice.d <= 0)
    {
        innerResult().code(LIQUIDITY_POOL_DEPOSIT_MALFORMED);
        return false;
    }

    if (mLiquidityPoolDeposit.maxPrice.n <= 0 ||
        mLiquidityPoolDeposit.maxPrice.d <= 0)
    {
        innerResult().code(LIQUIDITY_POOL_DEPOSIT_MALFORMED);
        return false;
    }

    // min.n * max.d > min.d * max.n is equivalent in arbitrary precision to
    // min.n / min.d > max.n / max.d but the first form avoids rounding in
    // finite precision.
    if ((int64_t)mLiquidityPoolDeposit.minPrice.n *
            (int64_t)mLiquidityPoolDeposit.maxPrice.d >
        (int64_t)mLiquidityPoolDeposit.minPrice.d *
            (int64_t)mLiquidityPoolDeposit.maxPrice.n)
    {
        innerResult().code(LIQUIDITY_POOL_DEPOSIT_MALFORMED);
        return false;
    }

    return true;
}

void
LiquidityPoolDepositOpFrame::insertLedgerKeysToPrefetch(
    UnorderedSet<LedgerKey>& keys) const
{
    keys.emplace(liquidityPoolKey(mLiquidityPoolDeposit.liquidityPoolID));
    keys.emplace(poolShareTrustLineKey(getSourceID(),
                                       mLiquidityPoolDeposit.liquidityPoolID));
}
}
