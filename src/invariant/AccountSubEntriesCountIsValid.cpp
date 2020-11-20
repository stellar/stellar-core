// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/AccountSubEntriesCountIsValid.h"
#include "invariant/InvariantManager.h"
#include "ledger/LedgerTxn.h"
#include "main/Application.h"
#include "util/Logging.h"
#include "util/UnorderedMap.h"
#include <fmt/format.h>

namespace stellar
{

static int32_t
calculateDelta(LedgerEntry const* current, LedgerEntry const* previous)
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

static void
updateChangedSubEntriesCount(
    UnorderedMap<AccountID, SubEntriesChange>& subEntriesChange,
    LedgerEntry const* current, LedgerEntry const* previous)
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
    case CLAIMABLE_BALANCE:
    {
        // claimable balance is not a subentry
        break;
    }
    default:
        abort();
    }
}

static void
updateChangedSubEntriesCount(
    UnorderedMap<AccountID, SubEntriesChange>& subEntriesChange,
    std::shared_ptr<InternalLedgerEntry const> const& genCurrent,
    std::shared_ptr<InternalLedgerEntry const> const& genPrevious)
{
    auto type = genCurrent ? genCurrent->type() : genPrevious->type();
    if (type == InternalLedgerEntryType::LEDGER_ENTRY)
    {
        auto const* current = genCurrent ? &genCurrent->ledgerEntry() : nullptr;
        auto const* previous =
            genPrevious ? &genPrevious->ledgerEntry() : nullptr;
        updateChangedSubEntriesCount(subEntriesChange, current, previous);
    }
}

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
    LedgerTxnDelta const& ltxDelta)
{
    UnorderedMap<AccountID, SubEntriesChange> subEntriesChange;
    for (auto const& entryDelta : ltxDelta.entry)
    {
        updateChangedSubEntriesCount(subEntriesChange,
                                     entryDelta.second.current,
                                     entryDelta.second.previous);
    }

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

    for (auto const& entryDelta : ltxDelta.entry)
    {
        if (entryDelta.second.current)
        {
            continue;
        }
        assert(entryDelta.second.previous);

        auto const& genPrevious = *entryDelta.second.previous;
        if (genPrevious.type() != InternalLedgerEntryType::LEDGER_ENTRY)
        {
            continue;
        }

        auto const& previous = genPrevious.ledgerEntry();
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
}
