// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/ConservationOfLumens.h"
#include "invariant/InvariantManager.h"
#include "ledger/LedgerHeaderReference.h"
#include "ledger/LedgerState.h"
#include "lib/util/format.h"
#include "main/Application.h"
#include <numeric>

#include "util/Logging.h"
#include "xdrpp/printer.h"

namespace stellar
{

ConservationOfLumens::ConservationOfLumens() : Invariant(false)
{
}

std::shared_ptr<Invariant>
ConservationOfLumens::registerInvariant(Application& app)
{
    return app.getInvariantManager().registerInvariant<ConservationOfLumens>();
}

std::string
ConservationOfLumens::getName() const
{
    return "ConservationOfLumens";
}

int64_t
ConservationOfLumens::calculateDeltaBalance(
    std::shared_ptr<LedgerEntry const> const& current,
    std::shared_ptr<LedgerEntry const> const& previous) const
{
    assert(current || previous);
    auto let = current ? current->data.type() : previous->data.type();
    if (let == ACCOUNT)
    {
        return (current ? current->data.account().balance : 0) -
               (previous ? previous->data.account().balance : 0);
    }
    return 0;
}

std::string
ConservationOfLumens::checkOnOperationApply(Operation const& operation,
                                            OperationResult const& result,
                                            LedgerState& ls)
{
    auto header = ls.loadHeader();
    int64_t deltaTotalCoins = header->header().totalCoins -
                              header->previousHeader().totalCoins;
    int64_t deltaFeePool = header->header().feePool -
                           header->previousHeader().feePool;
    header->invalidate();

    int64_t deltaBalances = std::accumulate(
        ls.begin(), ls.end(), static_cast<int64_t>(0),
        [this](int64_t lhs, LedgerState::IteratorValueType const& rhs) {
            return lhs + calculateDeltaBalance(rhs.entry(), rhs.previousEntry());
        });

    if (result.tr().type() == INFLATION)
    {
        int64_t inflationPayouts =
            std::accumulate(result.tr().inflationResult().payouts().begin(),
                            result.tr().inflationResult().payouts().end(),
                            static_cast<int64_t>(0),
                            [](int64_t lhs, InflationPayout const& rhs) {
                                return lhs + rhs.amount;
                            });
        if (deltaTotalCoins != inflationPayouts + deltaFeePool)
        {
            return fmt::format(
                "LedgerHeader totalCoins change ({}) did not match"
                " feePool change ({}) plus inflation payouts ({})",
                deltaTotalCoins, deltaFeePool, inflationPayouts);
        }
        if (deltaBalances != inflationPayouts)
        {
            return fmt::format("LedgerEntry account balances change ({}) "
                               "did not match inflation payouts ({})",
                               deltaBalances, inflationPayouts);
        }
    }
    else
    {
        if (deltaTotalCoins != 0)
        {
            auto header = ls.loadHeader();
            return fmt::format("LedgerHeader totalCoins changed from {} to"
                               " {} without inflation",
                               header->previousHeader().totalCoins,
                               header->header().totalCoins);
            header->invalidate();
        }
        if (deltaFeePool != 0)
        {
            auto header = ls.loadHeader();
            return fmt::format("LedgerHeader feePool changed from {} to"
                               " {} without inflation",
                               header->previousHeader().feePool,
                               header->header().feePool);
            header->invalidate();
        }
        if (deltaBalances != 0)
        {
            for (auto const& state : ls)
            {
                if (state.entry() && state.entry()->data.type() == ACCOUNT)
                {
                    if (state.previousEntry())
                    {
                        CLOG(INFO, "Ledger") << xdr::xdr_to_string(state.previousEntry()->data.account());
                    }
                    CLOG(INFO, "Ledger") << xdr::xdr_to_string(state.entry()->data.account());
                }
            }
            return fmt::format("LedgerEntry account balances changed by"
                               " {} without inflation",
                               deltaBalances);
        }
    }
    return {};
}
}
