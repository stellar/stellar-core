// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/MinimumAccountBalance.h"
#include "invariant/InvariantManager.h"
#include "ledger/LedgerHeaderReference.h"
#include "ledger/LedgerState.h"
#include "lib/util/format.h"
#include "main/Application.h"
#include "transactions/TransactionUtils.h"
#include "xdrpp/printer.h"

namespace stellar
{

std::shared_ptr<Invariant>
MinimumAccountBalance::registerInvariant(Application& app)
{
    return app.getInvariantManager().registerInvariant<MinimumAccountBalance>();
}

MinimumAccountBalance::MinimumAccountBalance()
    : Invariant(false)
{
}

std::string
MinimumAccountBalance::getName() const
{
    return "MinimumAccountBalance";
}

std::string
MinimumAccountBalance::checkOnOperationApply(Operation const& operation,
                                             OperationResult const& result,
                                             LedgerState& ls)
{
    auto header = ls.loadHeader();
    for (auto const& state : ls)
    {
        if (shouldCheckBalance(state))
        {
            auto const& account = state.entry()->data.account();
            auto minBalance =
                getCurrentMinBalance(header, account.numSubEntries);
            if (account.balance < minBalance)
            {
                header->invalidate();
                return fmt::format("Account does not meet the minimum "
                                   "balance requirement: {}",
                                   xdr::xdr_to_string(*state.entry()));
            }
        }
    }
    header->invalidate();
    return {};
}

bool
MinimumAccountBalance::shouldCheckBalance(
    LedgerState::IteratorValueType const& val) const
{
    if (!val.entry())
    {
        return false;
    }

    auto const& current = *val.entry();
    if (current.data.type() == ACCOUNT)
    {
        if (val.previousEntry())
        {
            auto const& previous = *val.previousEntry();
            return current.data.account().balance < previous.data.account().balance;
        }
        else
        {
            return true;
        }
    }
    return false;
}
}
