// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/ConstantProductInvariant.h"
#include "invariant/InvariantManager.h"
#include "ledger/InternalLedgerEntry.h"
#include "ledger/LedgerTxn.h"
#include "lib/util/uint128_t.h"
#include "main/Application.h"
#include <fmt/format.h>

namespace stellar
{

std::shared_ptr<Invariant>
ConstantProductInvariant::registerInvariant(Application& app)
{
    return app.getInvariantManager()
        .registerInvariant<ConstantProductInvariant>();
}

std::string
ConstantProductInvariant::getName() const
{
    return "ConstantProductInvariant";
}

bool
validateConstantProduct(uint64_t currentReserveA, uint64_t currentReserveB,
                        uint64_t previousReserveA, uint64_t previousReserveB)
{
    return (uint128_t)currentReserveA * (uint128_t)currentReserveB >=
           (uint128_t)previousReserveA * (uint128_t)previousReserveB;
}

std::string
ConstantProductInvariant::checkOnOperationApply(
    Operation const& operation, OperationResult const& result,
    LedgerTxnDelta const& ltxDelta, std::vector<ContractEvent> const& events)
{
    if (operation.body.type() == LIQUIDITY_POOL_WITHDRAW ||
        operation.body.type() == SET_TRUST_LINE_FLAGS ||
        operation.body.type() == ALLOW_TRUST)
    {
        return {};
    }

    for (auto const& entryDelta : ltxDelta.entry)
    {
        auto const& genCurrent = entryDelta.second.current;
        auto const& genPrevious = entryDelta.second.previous;
        if (!genCurrent || !genPrevious)
        {
            continue;
        }

        if (genCurrent->type() != InternalLedgerEntryType::LEDGER_ENTRY ||
            genCurrent->ledgerEntry().data.type() != LIQUIDITY_POOL)
        {
            continue;
        }

        auto const& poolCurrent = genCurrent->ledgerEntry()
                                      .data.liquidityPool()
                                      .body.constantProduct();
        auto const& poolPrevious = genPrevious->ledgerEntry()
                                       .data.liquidityPool()
                                       .body.constantProduct();

        // also checked in LedgerEntryIsValid
        if (poolCurrent.reserveA < 0 || poolCurrent.reserveB < 0 ||
            poolPrevious.reserveA < 0 || poolPrevious.reserveB < 0)
        {
            return "negative reserves";
        }

        if (!validateConstantProduct(poolCurrent.reserveA, poolCurrent.reserveB,
                                     poolPrevious.reserveA,
                                     poolPrevious.reserveB))
        {
            return fmt::format("Constant product invariant violated. crA={}, "
                               "crB={}, prA={}, prB={}",
                               poolCurrent.reserveA, poolCurrent.reserveB,
                               poolPrevious.reserveA, poolPrevious.reserveB);
        }
    }

    return {};
}
}