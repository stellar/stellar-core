// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/AccountSubEntriesCountIsValid.h"
#include "invariant/InvariantManager.h"
#include "ledger/LedgerDelta.h"
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
    LedgerDelta const& delta)
{
    std::unordered_map<AccountID, SubEntriesChange> subEntriesChange;
    countChangedSubEntries(subEntriesChange, delta.added().begin(),
                           delta.added().end());
    countChangedSubEntries(subEntriesChange, delta.modified().begin(),
                           delta.modified().end());
    countChangedSubEntries(subEntriesChange, delta.deleted().begin(),
                           delta.deleted().end());

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

    for (auto const& del : delta.deleted())
    {
        auto const& previous = del.previous->mEntry;
        if (previous.data.type() == ACCOUNT)
        {
            auto const& account = previous.data.account();
            auto const& change = subEntriesChange[account.accountID];
            int32_t numSigners =
                account.numSubEntries + change.numSubEntries - change.signers;
            if (numSigners != account.signers.size())
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
AccountSubEntriesCountIsValid::calculateDelta(LedgerEntry const* current,
                                              LedgerEntry const* previous) const
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
AccountSubEntriesCountIsValid::updateChangedSubEntriesCount(
    std::unordered_map<AccountID, SubEntriesChange>& subEntriesChange,
    LedgerEntry const* current, LedgerEntry const* previous) const
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

void
AccountSubEntriesCountIsValid::countChangedSubEntries(
    std::unordered_map<AccountID, SubEntriesChange>& subEntriesChange,
    LedgerDelta::AddedIterator iter,
    LedgerDelta::AddedIterator const& end) const
{
    for (; iter != end; ++iter)
    {
        updateChangedSubEntriesCount(subEntriesChange, &iter->current->mEntry,
                                     nullptr);
    }
}

void
AccountSubEntriesCountIsValid::countChangedSubEntries(
    std::unordered_map<AccountID, SubEntriesChange>& subEntriesChange,
    LedgerDelta::ModifiedIterator iter,
    LedgerDelta::ModifiedIterator const& end) const
{
    for (; iter != end; ++iter)
    {
        updateChangedSubEntriesCount(subEntriesChange, &iter->current->mEntry,
                                     &iter->previous->mEntry);
    }
}

void
AccountSubEntriesCountIsValid::countChangedSubEntries(
    std::unordered_map<AccountID, SubEntriesChange>& subEntriesChange,
    LedgerDelta::DeletedIterator iter,
    LedgerDelta::DeletedIterator const& end) const
{
    for (; iter != end; ++iter)
    {
        updateChangedSubEntriesCount(subEntriesChange, nullptr,
                                     &iter->previous->mEntry);
    }
}
}
