// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/AccountSubEntriesCountIsValid.h"
#include "invariant/InvariantManager.h"
#include "ledger/LedgerState.h"
#include "main/Application.h"
#include "util/Logging.h"
#include "util/format.h"
#include <unordered_map>

namespace stellar
{

AccountSubEntriesCountIsValid::AccountSubEntriesCountIsValid()
    : Invariant(false)
{
}

std::shared_ptr<Invariant>
AccountSubEntriesCountIsValid::registerInvariant(Application& app)
{
    return app.getInvariantManager()
        .registerInvariant<AccountSubEntriesCountIsValid>();
}

std::string
AccountSubEntriesCountIsValid::getName() const
{
    return "AccountSubEntriesCountIsValid";
}

std::string
AccountSubEntriesCountIsValid::checkOnOperationApply(
    Operation const& operation, OperationResult const& result,
    LedgerState const& ls, std::shared_ptr<LedgerHeaderReference const> header)
{
    std::unordered_map<AccountID, SubEntriesChange> subEntriesChange;
    countChangedSubEntries(subEntriesChange, ls);

    for (auto const& kv : subEntriesChange)
    {
        auto const& change = kv.second;
        if (change.numSubEntries != change.calculatedSubEntries)
        {
            return fmt::format(
                "Change in Account {} numSubEntries ({}) does not"
                " match change in number of subentries ({})",
                KeyUtils::toStrKey(kv.first), change.numSubEntries,
                change.calculatedSubEntries);
        }
    }

    for (auto const& state : ls)
    {
        if (state.entry())
        {
            continue;
        }

        auto const& previous = *state.previousEntry();
        if (previous.data.type() == ACCOUNT)
        {
            auto const& account = previous.data.account();
            auto const& change = subEntriesChange[account.accountID];
            int32_t numSigners =
                account.numSubEntries + change.numSubEntries - change.signers;
            if (numSigners != static_cast<int32_t>(account.signers.size()))
            {
                int32_t otherSubEntries =
                    static_cast<int32_t>(account.numSubEntries) -
                    static_cast<int32_t>(account.signers.size());
                return fmt::format(
                    "Deleted Account {} has {} subentries other than"
                    " signers",
                    KeyUtils::toStrKey(account.accountID), otherSubEntries);
            }
        }
    }
    return {};
}

int32_t
AccountSubEntriesCountIsValid::calculateDelta(
    std::shared_ptr<LedgerEntry const> const& current,
    std::shared_ptr<LedgerEntry const> const& previous) const
{
    int32_t delta = 0;
    if (current)
    {
        ++delta;
    }
    if (previous)
    {
        --delta;
    }
    return delta;
}

void
AccountSubEntriesCountIsValid::countChangedSubEntries(
    std::unordered_map<AccountID, SubEntriesChange>& subEntriesChange,
    LedgerState const& ls) const
{
    for (auto const& state : ls)
    {
        updateChangedSubEntriesCount(subEntriesChange,
                                     state.entry(),
                                     state.previousEntry());
    }
}

void
AccountSubEntriesCountIsValid::updateChangedSubEntriesCount(
    std::unordered_map<AccountID, SubEntriesChange>& subEntriesChange,
    std::shared_ptr<LedgerEntry const> const& current,
    std::shared_ptr<LedgerEntry const> const& previous) const
{
    auto valid = current ? current : previous;
    assert(valid);

    switch (valid->data.type())
    {
    case ACCOUNT:
    {
        auto accountID = valid->data.account().accountID;
        auto& change = subEntriesChange[accountID];
        change.numSubEntries =
            (current ? int32_t(current->data.account().numSubEntries) : 0) -
            (previous ? int32_t(previous->data.account().numSubEntries) : 0);
        change.signers =
            (current ? int32_t(current->data.account().signers.size()) : 0) -
            (previous ? int32_t(previous->data.account().signers.size()) : 0);
        change.calculatedSubEntries += change.signers;
        break;
    }
    case TRUSTLINE:
    {
        auto accountID = valid->data.trustLine().accountID;
        subEntriesChange[accountID].calculatedSubEntries +=
            calculateDelta(current, previous);
        break;
    }
    case OFFER:
    {
        auto accountID = valid->data.offer().sellerID;
        subEntriesChange[accountID].calculatedSubEntries +=
            calculateDelta(current, previous);
        break;
    }
    case DATA:
    {
        auto accountID = valid->data.data().accountID;
        subEntriesChange[accountID].calculatedSubEntries +=
            calculateDelta(current, previous);
        break;
    }
    default:
        abort();
    }
}
}
