// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/InvariantTestUtils.h"
#include "invariant/InvariantDoesNotHold.h"
#include "invariant/InvariantManager.h"
#include "ledger/EntryFrame.h"
#include "ledger/LedgerDelta.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTestUtils.h"
#include "lib/catch.hpp"
#include "main/Application.h"

namespace stellar
{
namespace InvariantTestUtils
{

LedgerEntry
generateRandomAccount(uint32_t ledgerSeq)
{
    LedgerEntry le;
    le.lastModifiedLedgerSeq = ledgerSeq;
    le.data.type(ACCOUNT);
    le.data.account() = LedgerTestUtils::generateValidAccountEntry(5);
    le.data.account().balance = 0;
    if (le.data.account().ext.v() > 0)
    {
        le.data.account().ext.v1().liabilities = Liabilities{0, 0};
    }
    return le;
}

bool
store(Application& app, UpdateList const& apply, LedgerDelta* ldPtr,
      OperationResult const* resPtr)
{
    LedgerHeader lh(app.getLedgerManager().getCurrentLedgerHeader());
    LedgerDelta ld(lh, app.getDatabase(), false);
    if (ldPtr == nullptr)
    {
        ldPtr = &ld;
    }
    for (auto const& toApply : apply)
    {
        auto& current = std::get<0>(toApply);
        auto& previous = std::get<1>(toApply);
        if (current && !previous)
        {
            current->storeAdd(*ldPtr, app.getDatabase());
        }
        else if (current && previous)
        {
            ldPtr->recordEntry(*previous);
            current->storeChange(*ldPtr, app.getDatabase());
        }
        else if (!current && previous)
        {
            ldPtr->recordEntry(*previous);
            previous->storeDelete(*ldPtr, app.getDatabase());
        }
        else
        {
            abort();
        }
    }

    OperationResult res;
    if (resPtr == nullptr)
    {
        resPtr = &res;
    }

    try
    {
        app.getInvariantManager().checkOnOperationApply({}, *resPtr, *ldPtr);
    }
    catch (InvariantDoesNotHold&)
    {
        return false;
    }
    return true;
}

UpdateList
makeUpdateList(EntryFrame::pointer left, EntryFrame::pointer right)
{
    UpdateList ul;
    ul.push_back(std::make_tuple(left, right));
    return ul;
}

std::vector<EntryFrame::pointer>
generateEntryFrames(std::vector<LedgerEntry> const& entries)
{
    std::vector<EntryFrame::pointer> result;
    std::transform(
        entries.begin(), entries.end(), std::back_inserter(result),
        [](LedgerEntry const& le) { return EntryFrame::FromXDR(le); });
    return result;
}

UpdateList
generateUpdateList(std::vector<EntryFrame::pointer> const& current,
                   std::vector<EntryFrame::pointer> const& previous)
{
    assert(current.size() == previous.size());
    UpdateList updates;
    std::transform(
        current.begin(), current.end(), previous.begin(),
        std::back_inserter(updates),
        [](EntryFrame::pointer const& curr, EntryFrame::pointer const& prev) {
            return UpdateList::value_type{curr, prev};
        });
    return updates;
}
}
}
