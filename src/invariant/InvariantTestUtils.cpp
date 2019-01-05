// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/InvariantTestUtils.h"
#include "invariant/InvariantDoesNotHold.h"
#include "invariant/InvariantManager.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTestUtils.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "transactions/TransactionUtils.h"

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
store(Application& app, UpdateList const& apply, AbstractLedgerTxn* ltxPtr,
      OperationResult const* resPtr)
{
    bool shouldCommit = !ltxPtr;
    std::unique_ptr<LedgerTxn> ltxStore;
    if (ltxPtr == nullptr)
    {
        ltxStore = std::make_unique<LedgerTxn>(app.getLedgerTxnRoot());
        ltxPtr = ltxStore.get();
    }
    for (auto const& toApply : apply)
    {
        auto& current = std::get<0>(toApply);
        auto& previous = std::get<1>(toApply);

        LedgerTxnEntry entry;
        if (previous)
        {
            entry = ltxPtr->load(LedgerEntryKey(*previous));
            if (current)
            {
                entry.current() = *current;
            }
            else
            {
                entry.erase();
            }
        }
        else if (current)
        {
            entry = ltxPtr->create(*current);
        }
        else
        {
            REQUIRE(false);
        }

        if (entry && entry.current().data.type() == ACCOUNT)
        {
            normalizeSigners(entry);
        }
    }

    OperationResult res;
    if (resPtr == nullptr)
    {
        resPtr = &res;
    }

    bool doInvariantsHold = true;
    try
    {
        app.getInvariantManager().checkOnOperationApply({}, *resPtr,
                                                        ltxPtr->getDelta());
    }
    catch (InvariantDoesNotHold&)
    {
        doInvariantsHold = false;
    }

    if (shouldCommit)
    {
        ltxPtr->commit();
    }
    return doInvariantsHold;
}

UpdateList
makeUpdateList(std::vector<LedgerEntry> const& current, std::nullptr_t previous)
{
    UpdateList updates;
    std::transform(current.begin(), current.end(), std::back_inserter(updates),
                   [](LedgerEntry const& curr) {
                       auto currPtr = std::make_shared<LedgerEntry>(curr);
                       return UpdateList::value_type{currPtr, nullptr};
                   });
    return updates;
}

UpdateList
makeUpdateList(std::vector<LedgerEntry> const& current,
               std::vector<LedgerEntry> const& previous)
{
    assert(current.size() == previous.size());
    UpdateList updates;
    std::transform(current.begin(), current.end(), previous.begin(),
                   std::back_inserter(updates),
                   [](LedgerEntry const& curr, LedgerEntry const& prev) {
                       auto currPtr = std::make_shared<LedgerEntry>(curr);
                       auto prevPtr = std::make_shared<LedgerEntry>(prev);
                       return UpdateList::value_type{currPtr, prevPtr};
                   });
    return updates;
}

UpdateList
makeUpdateList(std::nullptr_t current, std::vector<LedgerEntry> const& previous)
{
    UpdateList updates;
    std::transform(previous.begin(), previous.end(),
                   std::back_inserter(updates), [](LedgerEntry const& prev) {
                       auto prevPtr = std::make_shared<LedgerEntry>(prev);
                       return UpdateList::value_type{nullptr, prevPtr};
                   });
    return updates;
}
}
}
