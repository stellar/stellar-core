// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/InvariantTestUtils.h"
#include "invariant/InvariantDoesNotHold.h"
#include "invariant/InvariantManager.h"
#include "ledger/LedgerState.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTestUtils.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "util/make_unique.h"

#include "util/Logging.h"
#include "xdrpp/printer.h"

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
    return le;
}

bool
store(Application& app, UpdateList const& apply, LedgerState* lsPtr,
      OperationResult const* resPtr)
{
    bool shouldCommit = !lsPtr;
    std::unique_ptr<LedgerState> lsStore;
    if (!lsPtr)
    {
        lsStore = make_unique<LedgerState>(app.getLedgerStateRoot());
        lsPtr = lsStore.get();
    }

    for (auto const& toApply : apply)
    {
        auto current = std::get<0>(toApply);
        auto previous = std::get<1>(toApply);

        std::shared_ptr<LedgerEntryReference> ler;
        if (previous)
        {
            ler = lsPtr->load(LedgerEntryKey(*previous));
            if (current)
            {
                *ler->entry() = *current;
            }
            else
            {
                ler->erase();
            }
        }
        else if (current)
        {
            ler = lsPtr->create(*current);
        }
        else
        {
            abort();
        }

        if (current && current->data.type() == ACCOUNT)
        {
            AccountReference ar = ler;
            ar.normalizeSigners();
        }
    }

    OperationResult res;
    if (resPtr == nullptr)
    {
        resPtr = &res;
    }

    try
    {
        app.getInvariantManager().checkOnOperationApply({}, *resPtr, *lsPtr);
        if (shouldCommit)
        {
            lsPtr->commit();
        }
    }
    catch (InvariantDoesNotHold&)
    {
        if (shouldCommit)
        {
            lsPtr->commit();
        }
        return false;
    }
    return true;
}

UpdateList
makeUpdateList(LedgerEntry const* left, LedgerEntry const* right)
{
    UpdateList ul;
    ul.push_back(std::make_tuple(left, right));
    return ul;
}
}
}
