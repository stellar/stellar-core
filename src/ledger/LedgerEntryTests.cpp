// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Application.h"
#include "ledger/LedgerManager.h"
#include "util/Timer.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include "AccountFrame.h"
#include "LedgerDelta.h"
#include "xdrpp/marshal.h"
#include "xdrpp/autocheck.h"
#include "crypto/SecretKey.h"
#include "ledger/LedgerTestUtils.h"
#include "database/Database.h"
#include <utility>
#include <memory>
#include <unordered_map>

using namespace stellar;

namespace LedgerEntryTests
{

TEST_CASE("Account Entry tests", "[ledgerentry]")
{
    Config cfg(getTestConfig(0));

    VirtualClock clock;
    Application::pointer app = Application::create(clock, cfg);
    app->start();
    Database& db = app->getDatabase();

    SECTION("round trip with database")
    {
        std::vector<LedgerEntry> accounts(100);

        std::unordered_map<AccountID, LedgerEntry> accountsMap;

        for (auto& l : accounts)
        {
            l.data.type(ACCOUNT);
            auto& a = l.data.account();
            a = LedgerTestUtils::generateValidAccountEntry(5);
            accountsMap.insert(std::make_pair(a.accountID, l));
        }

        LedgerHeader lh;
        LedgerDelta delta(lh, db);

        // adding accounts
        for (auto const& l : accountsMap)
        {
            AccountFrame::pointer af = std::make_shared<AccountFrame>(l.second);
            af->storeAdd(delta, db);
            auto fromDb = AccountFrame::loadAccount(af->getID(), db);
            REQUIRE(af->getAccount() == fromDb->getAccount());
        }
        app->getLedgerManager().checkDbState();

        // updating accounts
        for (auto& l : accountsMap)
        {
            AccountEntry& newA = l.second.data.account();
            // replace by completely new object
            newA = LedgerTestUtils::generateValidAccountEntry(5);

            // preserve the accountID as it's the key
            newA.accountID = l.first;

            AccountFrame::pointer af = std::make_shared<AccountFrame>(l.second);
            af->storeChange(delta, db);
            auto fromDb = AccountFrame::loadAccount(af->getID(), db);
            REQUIRE(af->getAccount() == fromDb->getAccount());
        }
        app->getLedgerManager().checkDbState();

        // deleting accounts
        for (auto const& l : accountsMap)
        {
            AccountFrame::pointer af = std::make_shared<AccountFrame>(l.second);
            REQUIRE(AccountFrame::loadAccount(af->getID(), db) != nullptr);
            REQUIRE(AccountFrame::exists(db, af->getKey()));
            af->storeDelete(delta, db);
            REQUIRE(AccountFrame::loadAccount(af->getID(), db) == nullptr);
            REQUIRE(!AccountFrame::exists(db, af->getKey()));
        }
        app->getLedgerManager().checkDbState();
    }
}
}
