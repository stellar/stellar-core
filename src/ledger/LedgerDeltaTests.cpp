// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Timer.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "ledger/LedgerDelta.h"
#include "main/Application.h"
#include "LedgerTestUtils.h"
#include "ledger/LedgerManager.h"
#include "ledger/AccountFrame.h"

using namespace stellar;

TEST_CASE("Ledger delta", "[ledger][ledgerdelta]")
{
    Config cfg(getTestConfig());
    VirtualClock clock;
    Application::pointer app = Application::create(clock, cfg);
    app->start();
    LedgerHeader& curHeader = app->getLedgerManager().getCurrentLedgerHeader();
    LedgerHeader orgHeader = curHeader;

    LedgerDelta delta(curHeader, app->getDatabase());

    SECTION("header changes")
    {
        SECTION("commit top")
        {
            LedgerHeader expHeader = curHeader;
            expHeader.idPool++;

            SECTION("top")
            {
                delta.getHeader().idPool++;
                delta.commit();
            }
            SECTION("nested")
            {
                LedgerDelta delta2(delta);
                delta2.getHeader().idPool++;
                SECTION("inner no op")
                {
                    LedgerDelta delta3(delta2);
                }
                SECTION("inner rollback")
                {
                    LedgerDelta delta3(delta2);
                    delta3.getHeader().idPool++;
                    delta3.rollback();
                }
                delta2.commit();
                delta.commit();
            }
            SECTION("nested2")
            {
                LedgerDelta delta2(delta);
                {
                    LedgerDelta delta3(delta2);
                    delta3.getHeader().idPool++;
                    delta3.commit();
                }
                delta2.commit();
                delta.commit();
            }
            REQUIRE(curHeader == expHeader);
        }
        SECTION("rollback")
        {
            delta.getHeader().idPool++;
            delta.rollback();
            REQUIRE(curHeader == orgHeader);
        }
    }

    SECTION("delta object operations")
    {
        size_t const nbAccounts = 50;
        size_t const nbAccountsGroupSize = 10;

        std::vector<AccountFrame::pointer> accounts;
        {
            auto aEntries =
                LedgerTestUtils::generateValidAccountEntries(nbAccounts);
            accounts.reserve(nbAccounts);
            for (auto a : aEntries)
            {
                LedgerEntry le;
                le.data.type(ACCOUNT);
                le.data.account() = a;
                accounts.emplace_back(std::make_shared<AccountFrame>(le));
            }
        }

        using MapAccounts =
            std::map<LedgerKey, AccountFrame::pointer, LedgerEntryIdCmp>;

        MapAccounts accountsByKey;

        auto addEntries =
            [&](size_t start, size_t end, LedgerDelta& d, MapAccounts& aKeys)
        {
            for (size_t i = start; i < end; i++)
            {
                auto a = accounts[i];
                d.addEntry(*a);
                aKeys.insert(std::make_pair(a->getKey(), a));
            }
        };

        addEntries(0, nbAccountsGroupSize, delta, accountsByKey);

        auto checkChanges = [&]()
        {
            auto changes = delta.getChanges();
            for (auto const& c : changes)
            {
                REQUIRE(c.type() == LEDGER_ENTRY_CREATED);
                auto const& createdEntry = c.created();
                auto key = LedgerEntryKey(createdEntry);
                REQUIRE(createdEntry == (accountsByKey[key]->mEntry));
            }
        };

        checkChanges();

        SECTION("add more entries")
        {
            SECTION("commit")
            {
                LedgerDelta delta2(delta);
                addEntries(nbAccountsGroupSize, nbAccountsGroupSize * 2, delta2,
                           accountsByKey);
                delta2.commit();
                REQUIRE(accountsByKey.size() == (nbAccountsGroupSize * 2));
            }
            SECTION("rollback")
            {
                LedgerDelta delta2(delta);
                MapAccounts accountsByKey2;
                addEntries(nbAccountsGroupSize,
                           nbAccountsGroupSize + nbAccountsGroupSize, delta2,
                           accountsByKey2);
                delta2.rollback();
                REQUIRE(accountsByKey.size() == nbAccountsGroupSize);
            }
            checkChanges();
        }
        SECTION("modified entries")
        {
            auto modEntries = [&](size_t start, size_t end, LedgerDelta& d,
                                  MapAccounts& aKeys)
            {
                for (size_t i = start; i < end; i++)
                {
                    auto a = accounts[i];
                    SequenceNumber s = a->getSeqNum() + 1;
                    auto newA = std::make_shared<AccountFrame>(a->mEntry);
                    newA->setSeqNum(s);
                    d.modEntry(*newA);
                    aKeys[newA->getKey()] = newA;
                }
            };
            SECTION("commit")
            {
                LedgerDelta delta2(delta);
                MapAccounts modAccounts;
                modEntries(0, nbAccountsGroupSize / 2, delta2, modAccounts);

                auto changes = delta2.getChanges();
                REQUIRE(changes.size() == modAccounts.size());
                for (auto const& c : changes)
                {
                    REQUIRE(c.type() == LEDGER_ENTRY_UPDATED);
                    auto const& updatedEntry = c.updated();
                    auto key = LedgerEntryKey(updatedEntry);
                    REQUIRE(updatedEntry == (modAccounts[key]->mEntry));
                    // update accountsByKey so that we can check that everything
                    // is committed to the outer delta
                    accountsByKey[key] = modAccounts[key];
                }
                delta2.commit();
                REQUIRE(accountsByKey.size() == nbAccountsGroupSize);
            }
            SECTION("rollback")
            {
                LedgerDelta delta2(delta);
                MapAccounts modAccounts;
                modEntries(0, nbAccountsGroupSize / 2, delta2, modAccounts);
                delta2.rollback();
                REQUIRE(accountsByKey.size() == nbAccountsGroupSize);
            }
            checkChanges();
        }
        SECTION("deleted entries")
        {
            auto delEntries = [&](size_t start, size_t end, LedgerDelta& d,
                                  MapAccounts& aKeys)
            {
                for (size_t i = start; i < end; i++)
                {
                    auto a = accounts[i];
                    auto const& key = a->getKey();
                    d.deleteEntry(key);
                    aKeys[key] = nullptr;
                }
            };
            SECTION("commit")
            {
                LedgerDelta delta2(delta);
                MapAccounts delAccounts;
                delEntries(0, nbAccountsGroupSize / 2, delta2, delAccounts);

                auto changes = delta2.getChanges();
                REQUIRE(changes.size() == delAccounts.size());
                for (auto const& c : changes)
                {
                    REQUIRE(c.type() == LEDGER_ENTRY_REMOVED);
                    auto const& removedEntry = c.removed();
                    REQUIRE(delAccounts[removedEntry] == nullptr);
                    // update accountsByKey so that we can check that everything
                    // is committed to the outer delta
                    accountsByKey.erase(removedEntry);
                }
                delta2.commit();
                REQUIRE(accountsByKey.size() == (nbAccountsGroupSize / 2));
            }
            SECTION("rollback")
            {
                LedgerDelta delta2(delta);
                MapAccounts delAccounts;
                delEntries(0, nbAccountsGroupSize / 2, delta2, delAccounts);
                delta2.rollback();
                REQUIRE(accountsByKey.size() == nbAccountsGroupSize);
            }
            checkChanges();
        }
    }
}
