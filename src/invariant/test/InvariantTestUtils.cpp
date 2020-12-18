// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/test/InvariantTestUtils.h"
#include "invariant/InvariantDoesNotHold.h"
#include "invariant/InvariantManager.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/test/LedgerTestUtils.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "transactions/TransactionUtils.h"
#include <numeric>

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

        if (previous && !current)
        {
            REQUIRE_THROWS_AS(!entry, std::runtime_error);
        }
        else if (entry && entry.current().data.type() == ACCOUNT)
        {
            normalizeSigners(entry.current().data.account());
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

void
normalizeSigners(AccountEntry& acc)
{
    // Get indexes after sorting by acc.signer keys
    std::vector<std::size_t> indices(acc.signers.size());
    std::iota(indices.begin(), indices.end(), 0);
    std::sort(indices.begin(), indices.end(), [&acc](size_t i, size_t j) {
        return acc.signers[i] < acc.signers[j];
    });

    // Sort both vectors based on indices from above. If a signer in acc.signers
    // moves, the corresponding sponsoringID needs to move to the same index as
    // well
    xdr::xvector<Signer, MAX_SIGNERS> sortedSigners;
    xdr::xvector<SponsorshipDescriptor, MAX_SIGNERS> sortedSignerSponsoringIDs;
    bool aeIsV2 = hasAccountEntryExtV2(acc);

    for (size_t index : indices)
    {
        sortedSigners.emplace_back(acc.signers[index]);
        if (aeIsV2)
        {
            sortedSignerSponsoringIDs.emplace_back(
                acc.ext.v1().ext.v2().signerSponsoringIDs[index]);
        }
    }

    acc.signers.swap(sortedSigners);
    if (aeIsV2)
    {
        acc.ext.v1().ext.v2().signerSponsoringIDs.swap(
            sortedSignerSponsoringIDs);
    }
}

int64_t
getMinBalance(Application& app, AccountEntry const& acc)
{
    LedgerTxn ltx(app.getLedgerTxnRoot());
    return getMinBalance(ltx.loadHeader().current(), acc);
}
}
}
