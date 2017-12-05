// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/TotalCoinsEqualsBalancesPlusFeePool.h"
#include "database/AccountQueries.h"
#include "invariant/InvariantManager.h"
#include "ledger/LedgerDelta.h"
#include "lib/util/format.h"
#include "main/Application.h"

namespace stellar
{

std::shared_ptr<Invariant>
TotalCoinsEqualsBalancesPlusFeePool::registerInvariant(Application& app)
{
    return app.getInvariantManager()
        .registerInvariant<TotalCoinsEqualsBalancesPlusFeePool>(
            app.getDatabase());
}

TotalCoinsEqualsBalancesPlusFeePool::TotalCoinsEqualsBalancesPlusFeePool(
    Database& db)
    : Invariant(false), mDb{db}
{
}

std::string
TotalCoinsEqualsBalancesPlusFeePool::getName() const
{
    return "TotalCoinsEqualsBalancesPlusFeePool";
}

std::string
TotalCoinsEqualsBalancesPlusFeePool::checkOnLedgerClose(
    LedgerDelta const& delta)
{
    auto& lh = delta.getHeader();
    if (lh.ledgerVersion <= 7) // due to bugs in previous versions
    {
        return {};
    }

    auto ledgerTotalCoins = lh.totalCoins;
    auto feePool = lh.feePool;
    auto databaseTotalCoins = sumOfBalances(mDb);

    if (ledgerTotalCoins != databaseTotalCoins + feePool)
    {
        return fmt::format(
            "lh.totalCoins = {}, sum(balance) = {}, lh.feePool = {}",
            ledgerTotalCoins, databaseTotalCoins, feePool);
    }

    return {};
}
}
