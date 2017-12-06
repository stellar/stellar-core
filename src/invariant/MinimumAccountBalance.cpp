// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/MinimumAccountBalance.h"
#include "invariant/InvariantManager.h"
#include "ledger/LedgerDelta.h"
#include "ledger/LedgerManager.h"
#include "lib/util/format.h"
#include "main/Application.h"
#include "xdrpp/printer.h"

namespace stellar
{

std::shared_ptr<Invariant>
MinimumAccountBalance::registerInvariant(Application& app)
{
    return app.getInvariantManager().registerInvariant<MinimumAccountBalance>(
        app.getLedgerManager());
}

MinimumAccountBalance::MinimumAccountBalance(LedgerManager const& lm)
    : Invariant(false), mLedgerManager{lm}
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
                                             LedgerDelta const& delta)
{
    auto msg = checkAccountBalance(delta.added().begin(), delta.added().end());
    if (!msg.empty())
    {
        return msg;
    }

    msg = checkAccountBalance(delta.modified().begin(), delta.modified().end());
    if (!msg.empty())
    {
        return msg;
    }
    return {};
}

bool
MinimumAccountBalance::shouldCheckBalance(
    LedgerDelta::AddedLedgerEntry const& ale) const
{
    return ale.current->mEntry.data.type() == ACCOUNT;
}

bool
MinimumAccountBalance::shouldCheckBalance(
    LedgerDelta::ModifiedLedgerEntry const& mle) const
{
    auto const& current = mle.current->mEntry;
    if (current.data.type() == ACCOUNT)
    {
        auto const& previous = mle.previous->mEntry;
        assert(previous.data.type() == ACCOUNT);
        return current.data.account().balance < previous.data.account().balance;
    }
    return false;
}

template <typename IterType>
std::string
MinimumAccountBalance::checkAccountBalance(IterType iter,
                                           IterType const& end) const
{
    for (; iter != end; ++iter)
    {
        auto const& current = iter->current->mEntry;
        if (current.data.type() == ACCOUNT)
        {
            if (shouldCheckBalance(*iter))
            {
                auto const& account = current.data.account();
                auto minBalance =
                    mLedgerManager.getMinBalance(account.numSubEntries);
                if (account.balance < minBalance)
                {
                    return fmt::format("Account does not meet the minimum "
                                       "balance requirement: {}",
                                       xdr::xdr_to_string(current));
                }
            }
        }
    }
    return {};
}
}
