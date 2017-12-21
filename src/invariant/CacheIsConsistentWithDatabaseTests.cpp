// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "database/Database.h"
#include "invariant/CacheIsConsistentWithDatabase.h"
#include "invariant/InvariantDoesNotHold.h"
#include "invariant/InvariantManager.h"
#include "ledger/LedgerDelta.h"
#include "ledger/LedgerTestUtils.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include <random>

using namespace stellar;

namespace stellar
{
using xdr::operator<;
}

static LedgerEntry
generateRandomLedgerEntry(std::map<LedgerKey, LedgerEntry>& liveEntries,
                          uint32_t ledgerSeq, bool update)
{
    LedgerEntry le;
    do
    {
        le = LedgerTestUtils::generateValidLedgerEntry(5);
    } while (liveEntries.find(LedgerEntryKey(le)) != liveEntries.end());
    le.lastModifiedLedgerSeq = ledgerSeq;

    if (update)
    {
        liveEntries[LedgerEntryKey(le)] = le;
    }
    return le;
}

static LedgerEntry
generateRandomModifiedEntryFrame(std::map<LedgerKey, LedgerEntry>& liveEntries,
                                 uint32_t ledgerSeq,
                                 std::default_random_engine& gen)
{
    assert(liveEntries.size() > 0);
    std::uniform_int_distribution<uint32_t> dist(
        0, uint32_t(liveEntries.size()) - 1);
    auto iter = liveEntries.begin();
    std::advance(iter, dist(gen));

    LedgerEntry le;
    do
    {
        le = generateRandomLedgerEntry(liveEntries, ledgerSeq, false);
    } while (le.data.type() != iter->second.data.type());

    auto const& previous = iter->second;
    switch (le.data.type())
    {
    case ACCOUNT:
        le.data.account().accountID = previous.data.account().accountID;
        break;
    case OFFER:
        le.data.offer().sellerID = previous.data.offer().sellerID;
        le.data.offer().offerID = previous.data.offer().offerID;
        break;
    case TRUSTLINE:
        le.data.trustLine().accountID = previous.data.trustLine().accountID;
        le.data.trustLine().asset = previous.data.trustLine().asset;
        break;
    case DATA:
        le.data.data().accountID = previous.data.data().accountID;
        le.data.data().dataName = previous.data.data().dataName;
        break;
    default:
        abort();
    }

    iter->second = le;
    return le;
}

static LedgerEntry
deleteRandomLedgerEntry(std::map<LedgerKey, LedgerEntry>& liveEntries,
                        std::default_random_engine& gen)
{
    assert(liveEntries.size() > 0);
    std::uniform_int_distribution<uint32_t> dist(
        0, uint32_t(liveEntries.size()) - 1);
    auto iter = liveEntries.begin();
    std::advance(iter, dist(gen));

    auto le = iter->second;
    liveEntries.erase(iter);
    return le;
}

static void
generateLedger(Application& app, LedgerDelta& ld,
               std::map<LedgerKey, LedgerEntry>& liveEntries,
               uint32_t ledgerSeq, uint32_t nUpdates,
               std::default_random_engine& gen)
{
    std::map<LedgerKey, LedgerEntry> previousState = liveEntries;
    std::uniform_int_distribution<int32_t> changesDist(-1, 2);

    for (uint32_t j = 0; j < nUpdates || previousState == liveEntries; ++j)
    {
        auto change = changesDist(gen);
        if (change > 0 || liveEntries.size() == 0)
        {
            auto le = generateRandomLedgerEntry(liveEntries, 2 + j, true);
            auto ef = EntryFrame::FromXDR(le);
            ef->storeAdd(ld, app.getDatabase());
        }
        else if (change == 0)
        {
            auto le = generateRandomModifiedEntryFrame(liveEntries, 2 + j, gen);
            auto ef = EntryFrame::FromXDR(le);
            ef->storeChange(ld, app.getDatabase());
        }
        else if (change < 0)
        {
            auto le = deleteRandomLedgerEntry(liveEntries, gen);
            auto ef = EntryFrame::FromXDR(le);
            ef->storeDelete(ld, app.getDatabase());
        }
    }
}

TEST_CASE("Check cache is consistent", "[invariant][cacheisconsistent]")
{
    std::default_random_engine gen;
    Config cfg = getTestConfig(0);
    cfg.INVARIANT_CHECKS = {"CacheIsConsistentWithDatabase"};

    std::bernoulli_distribution dist(0.5);
    for (uint32_t i = 0; i < 100; ++i)
    {
        VirtualClock clock;
        Application::pointer app = createTestApplication(clock, cfg);

        std::map<LedgerKey, LedgerEntry> liveEntries;
        for (uint32_t j = 0; j < 50; ++j)
        {
            LedgerHeader lh(app->getLedgerManager().getCurrentLedgerHeader());
            LedgerDelta ld(lh, app->getDatabase(), false);
            OperationResult res;

            if (dist(gen))
            {
                generateLedger(*app, ld, liveEntries, 2 + j, 10, gen);
                REQUIRE_NOTHROW(
                    app->getInvariantManager().checkOnOperationApply({}, res,
                                                                     ld));
            }
            else
            {
                {
                    soci::transaction sqlTx(app->getDatabase().getSession());
                    auto liveEntriesCopy = liveEntries;
                    generateLedger(*app, ld, liveEntriesCopy, 2 + j, 10, gen);
                }
                OperationResult res2;
                REQUIRE_THROWS_AS(
                    app->getInvariantManager().checkOnOperationApply({}, res2,
                                                                     ld),
                    InvariantDoesNotHold);
            }
        }
    }
}
