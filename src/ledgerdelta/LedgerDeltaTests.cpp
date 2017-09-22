// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/LedgerCmp.h"
#include "database/EntryQueries.h"
#include "ledger/AccountFrame.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTestUtils.h"
#include "ledgerdelta/LedgerDelta.h"
#include "ledgerdelta/LedgerDeltaLayer.h"
#include "ledgerdelta/LedgerDeltaScope.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "test/test.h"
#include "util/Timer.h"
#include "util/make_unique.h"
#include "xdr/Stellar-SCP.h"
#include "xdr/Stellar-ledger.h"
#include "xdrpp/printer.h"

using namespace stellar;

namespace Catch
{

template <>
std::string
toString(LedgerHeader const& hdr)
{
    return xdr::xdr_to_string(hdr);
}
}

TEST_CASE("Ledger delta", "[ledger][ledgerdelta]")
{
    Config cfg(getTestConfig());
    VirtualClock clock;
    Application::pointer app = Application::create(clock, cfg);
    app->start();
    LedgerHeader& curHeader = app->getLedgerManager().getCurrentLedgerHeader();
    LedgerHeader orgHeader = curHeader;

    auto ledgerDelta = LedgerDelta{curHeader, app->getLedgerEntries()};

    SECTION("header changes")
    {
        SECTION("apply top")
        {
            LedgerHeader expHeader = curHeader;
            expHeader.idPool++;

            SECTION("top")
            {
                ledgerDelta.getHeader().idPool++;
                app->getLedgerManager().apply(ledgerDelta);
                REQUIRE(app->getLedgerManager().getCurrentLedgerHeader() ==
                        expHeader);
            }
            SECTION("nested")
            {
                {
                    LedgerDeltaScope nested{ledgerDelta};
                    ledgerDelta.getHeader().idPool++;
                    SECTION("inner no op")
                    {
                        LedgerDeltaScope nested2{ledgerDelta};
                    }
                    nested.commit();
                }
                app->getLedgerManager().apply(ledgerDelta);
                REQUIRE(app->getLedgerManager().getCurrentLedgerHeader() ==
                        expHeader);
            }
            SECTION("nested2")
            {
                {
                    LedgerDeltaScope nested{ledgerDelta};
                    {
                        LedgerDeltaScope nested2{ledgerDelta};
                        ledgerDelta.getHeader().idPool++;
                        nested2.commit();
                    }
                    nested.commit();
                }
                app->getLedgerManager().apply(ledgerDelta);
                REQUIRE(app->getLedgerManager().getCurrentLedgerHeader() ==
                        expHeader);
            }
        }
        SECTION("no commit")
        {
            ledgerDelta.getHeader().idPool++;
            REQUIRE(app->getLedgerManager().getCurrentLedgerHeader() ==
                    orgHeader);
        }
    }

    SECTION("delta object operations")
    {
        size_t const nbAccounts = 36;
        size_t const nbAccountsGroupSize = 9;

        using MapAccounts = std::map<LedgerKey, LedgerEntry, LedgerEntryIdCmp>;

        MapAccounts orgAccounts;
        std::vector<LedgerEntry> accounts;
        {
            uint32 s = ledgerDelta.getHeader().ledgerSeq;
            auto aEntries =
                LedgerTestUtils::generateValidAccountEntries(nbAccounts);
            accounts.reserve(nbAccounts);
            for (auto a : aEntries)
            {
                auto account = AccountFrame{a};
                account.setSeqNum(s);
                accounts.emplace_back(account.getEntry());
                orgAccounts.emplace(
                    std::make_pair(account.getKey(), account.getEntry()));
            }
        }

        MapAccounts accountsByKey;

        auto addEntries = [&](size_t start, size_t end, LedgerDelta& d,
                              MapAccounts& aKeys) {
            for (size_t i = start; i < end; i++)
            {
                auto a = EntryFrame{accounts.at(i)};
                a.getEntry().lastModifiedLedgerSeq = d.getHeader().ledgerSeq;
                d.addEntry(a);
                aKeys.insert(std::make_pair(a.getKey(), a.getEntry()));
            }
        };

        auto modEntries = [&](size_t start, size_t end, LedgerDelta& d,
                              MapAccounts& aKeys) {
            for (size_t i = start; i < end; i++)
            {
                auto a = AccountFrame{accounts.at(i)};
                auto key = a.getKey();
                auto it = aKeys.find(key);
                if (it != aKeys.end())
                {
                    a = it->second;
                }
                d.recordEntry(a);
                auto s = a.getSeqNum() + 1;
                a.setSeqNum(s);
                a.getEntry().lastModifiedLedgerSeq = d.getHeader().ledgerSeq;
                d.updateEntry(a);
                aKeys[key] = a.getEntry();
            }
        };

        auto delEntries = [&](size_t start, size_t end, LedgerDelta& d,
                              MapAccounts& aKeys) {
            for (size_t i = start; i < end; i++)
            {
                auto a = AccountFrame{accounts.at(i)};
                auto key = a.getKey();
                auto it = aKeys.find(key);
                if (it != aKeys.end())
                {
                    a = it->second;
                }
                d.recordEntry(a);
                d.deleteEntry(key);
                aKeys.erase(key);
            }
        };

        ledgerDelta.getHeader().ledgerSeq++;

        // builds a delta containing
        // [adds N][mods N][dels N]

        // add entries to mod/remove
        for (auto i = nbAccountsGroupSize; i < nbAccountsGroupSize * 3; i++)
        {
            insertEntry(accounts.at(i), app->getDatabase());
        }

        // add entries to the top level delta
        addEntries(0, nbAccountsGroupSize, ledgerDelta, accountsByKey);
        // modify entries
        modEntries(nbAccountsGroupSize, nbAccountsGroupSize * 2, ledgerDelta,
                   accountsByKey);
        // delete entries
        delEntries(nbAccountsGroupSize * 2, nbAccountsGroupSize * 3,
                   ledgerDelta, accountsByKey);

        auto checkChanges = [&](LedgerDeltaLayer& d, size_t nbAdds,
                                size_t nbMods, size_t nbDels, size_t nbStates,
                                MapAccounts const& orgData) {
            auto changes = d.getChanges();
            size_t expectedChanges = nbAdds + nbMods + nbDels + nbStates;
            size_t adds = 0, mods = 0, dels = 0, states = 0;

            bool gotState = false;
            LedgerKey stateKey;
            for (auto const& c : changes)
            {
                switch (c.type())
                {
                case LEDGER_ENTRY_CREATED:
                {
                    REQUIRE(!gotState);
                    auto const& createdEntry = c.created();
                    auto key = entryKey(createdEntry);
                    REQUIRE(createdEntry == accountsByKey.at(key));
                    adds++;
                }
                break;
                case LEDGER_ENTRY_REMOVED:
                {
                    auto const& removedEntry = c.removed();
                    if (gotState)
                    {
                        REQUIRE(stateKey == removedEntry);
                        gotState = false;
                    }
                    REQUIRE(accountsByKey.find(removedEntry) ==
                            std::end(accountsByKey));
                    dels++;
                }
                break;
                case LEDGER_ENTRY_UPDATED:
                {
                    auto const& updatedEntry = c.updated();
                    auto key = entryKey(updatedEntry);
                    if (gotState)
                    {
                        REQUIRE(key == stateKey);
                        gotState = false;
                    }
                    REQUIRE(updatedEntry == accountsByKey.at(key));
                    mods++;
                }
                break;
                case LEDGER_ENTRY_STATE:
                {
                    REQUIRE(!gotState);
                    gotState = true;
                    auto const& state = c.state();
                    auto key = entryKey(state);
                    stateKey = key;
                    REQUIRE(state == orgData.at(key));
                    states++;
                }
                break;
                }
            }
            REQUIRE(changes.size() == expectedChanges);
            REQUIRE(!gotState);
            REQUIRE(adds == nbAdds);
            REQUIRE(mods == nbMods);
            REQUIRE(dels == nbDels);
            REQUIRE(states == nbStates);
        };

        checkChanges(ledgerDelta.top(), nbAccountsGroupSize,
                     nbAccountsGroupSize, nbAccountsGroupSize,
                     nbAccountsGroupSize * 2, orgAccounts);

        MapAccounts orgAccountsBeforeD2 = accountsByKey;
        orgAccountsBeforeD2.insert(orgAccounts.begin(), orgAccounts.end());

        SECTION("add more entries")
        {
            SECTION("apply")
            {
                {
                    LedgerDeltaScope nested{ledgerDelta};
                    addEntries(nbAccountsGroupSize * 3, nbAccountsGroupSize * 4,
                               ledgerDelta, accountsByKey);
                    nested.commit();
                }
                checkChanges(ledgerDelta.top(), nbAccountsGroupSize * 2,
                             nbAccountsGroupSize, nbAccountsGroupSize,
                             nbAccountsGroupSize * 2, orgAccounts);
            }
            SECTION("rollback")
            {
                {
                    LedgerDeltaScope nested{ledgerDelta};
                    MapAccounts accountsByKey2;
                    addEntries(nbAccountsGroupSize * 3, nbAccountsGroupSize * 4,
                               ledgerDelta, accountsByKey2);
                }
                checkChanges(ledgerDelta.top(), nbAccountsGroupSize,
                             nbAccountsGroupSize, nbAccountsGroupSize,
                             nbAccountsGroupSize * 2, orgAccounts);
            }
        }
        SECTION("modified entries")
        {
            // add entries to mod/remove
            for (auto i = nbAccountsGroupSize * 3; i < nbAccountsGroupSize * 4; i++)
            {
                insertEntry(accounts.at(i), app->getDatabase());
            }

            auto nested = make_unique<LedgerDeltaScope>(ledgerDelta);
            MapAccounts modAccounts = accountsByKey;

            // modify entries that were added and modified
            size_t start = nbAccountsGroupSize * 2 / 3;
            modEntries(start, start + nbAccountsGroupSize, ledgerDelta,
                       modAccounts);
            // add modified entries that were not tracked so far
            modEntries(nbAccountsGroupSize * 3, nbAccountsGroupSize * 4,
                       ledgerDelta, modAccounts);

            SECTION("apply")
            {
                accountsByKey = modAccounts;
                checkChanges(ledgerDelta.top(), 0, nbAccountsGroupSize * 2, 0,
                             nbAccountsGroupSize, orgAccountsBeforeD2);
                nested->commit();
                nested = nullptr; // do the commit
                checkChanges(ledgerDelta.top(), nbAccountsGroupSize,
                             nbAccountsGroupSize * 2, nbAccountsGroupSize,
                             nbAccountsGroupSize * 3, orgAccounts);
            }
            SECTION("rollback")
            {
                nested = nullptr; // rollback
                checkChanges(ledgerDelta.top(), nbAccountsGroupSize,
                             nbAccountsGroupSize, nbAccountsGroupSize,
                             nbAccountsGroupSize * 2, orgAccounts);
            }
        }
        SECTION("deleted entries")
        {
            // add entries to mod/remove
            for (auto i = nbAccountsGroupSize * 3; i < nbAccountsGroupSize * 4; i++)
            {
                insertEntry(accounts.at(i), app->getDatabase());
            }

            auto nested = make_unique<LedgerDeltaScope>(ledgerDelta);
            MapAccounts delAccounts = accountsByKey;

            // delete entries that were added and modified
            size_t start = nbAccountsGroupSize * 2 / 3;
            delEntries(start, start + nbAccountsGroupSize, ledgerDelta,
                       delAccounts);
            // add deleted entries that were not tracked so far
            delEntries(nbAccountsGroupSize * 3, nbAccountsGroupSize * 4,
                       ledgerDelta, delAccounts);

            SECTION("apply")
            {
                accountsByKey = delAccounts;
                checkChanges(ledgerDelta.top(), 0, 0, nbAccountsGroupSize * 2,
                             nbAccountsGroupSize, orgAccountsBeforeD2);
                nested->commit();
                nested = nullptr; // do the commit
                // adds/mods were replaced by a delete
                // adds+del result in no-op
                size_t adds2del = nbAccountsGroupSize / 3;
                checkChanges(ledgerDelta.top(), nbAccountsGroupSize - adds2del,
                             nbAccountsGroupSize - start,
                             nbAccountsGroupSize * 3 - adds2del,
                             nbAccountsGroupSize * 3, orgAccounts);
            }
            SECTION("rollback")
            {
                nested = nullptr; // rollback
                checkChanges(ledgerDelta.top(), nbAccountsGroupSize,
                             nbAccountsGroupSize, nbAccountsGroupSize,
                             nbAccountsGroupSize * 2, orgAccounts);
            }
        }
    }
}
