// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Timer.h"
#include "main/Application.h"
#include "main/test.h"
#include "main/Config.h"
#include "lib/catch.hpp"
#include "database/Database.h"
#include "ledger/LedgerDelta.h"
#include "ledger/LedgerManager.h"
#include "ledger/EntryFrame.h"
#include "util/Logging.h"
#include "util/types.h"
#include <xdrpp/autocheck.h>

using namespace stellar;

template <typename T>
void
clampLow(T low, T& v)
{
    if (v < low)
    {
        v = low;
    }
}

template <typename T>
void
clampHigh(T high, T& v)
{
    if (v > high)
    {
        v = high;
    }
}

auto validLedgerEntryGenerator = autocheck::map(
    [](LedgerEntry&& le, size_t s)
    {
        switch (le.type())
        {
        case TRUSTLINE:
        {
            le.trustLine().currency.type(ISO4217);
            strToCurrencyCode(le.trustLine().currency.isoCI().currencyCode,
                              "USD");
            clampLow<int64_t>(0, le.trustLine().balance);
            clampLow<int64_t>(0, le.trustLine().limit);
            clampHigh<int64_t>(le.trustLine().limit, le.trustLine().balance);
        }
        break;

        case OFFER:
        {
            le.offer().takerGets.type(ISO4217);
            strToCurrencyCode(le.offer().takerGets.isoCI().currencyCode, "CAD");

            le.offer().takerPays.type(ISO4217);
            strToCurrencyCode(le.offer().takerPays.isoCI().currencyCode, "EUR");

            clampLow<int64_t>(0, le.offer().amount);
            clampLow(0, le.offer().price.n);
            clampLow(1, le.offer().price.d);
        }
        break;

        case ACCOUNT:
        {
            clampLow<int64_t>(0, le.account().balance);
            break;
        }
        }

        return le;
    },
    autocheck::generator<LedgerEntry>());

LedgerEntry
generateValidLedgerEntry()
{
    return validLedgerEntryGenerator(10);
}

TEST_CASE("Ledger entry db lifecycle", "[ledger]")
{
    Config cfg(getTestConfig());
    VirtualClock clock;
    Application::pointer app = Application::create(clock, cfg);

    app->start();
    LedgerDelta delta(app->getLedgerManager().getCurrentLedgerHeader());
    auto& db = app->getDatabase();
    for (size_t i = 0; i < 100; ++i)
    {
        auto le = EntryFrame::FromXDR(validLedgerEntryGenerator(3));
        CHECK(!EntryFrame::exists(db, le->getKey()));
        le->storeAddOrChange(delta, db);
        CHECK(EntryFrame::exists(db, le->getKey()));
        le->storeDelete(delta, db);
        CHECK(!EntryFrame::exists(db, le->getKey()));
    }
}

TEST_CASE("single ledger entry insert SQL", "[singlesql][entrysql][hide]")
{
    Config::TestDbMode mode = Config::TESTDB_ON_DISK_SQLITE;
#ifdef USE_POSTGRES
    mode = Config::TESTDB_TCP_LOCALHOST_POSTGRESQL;
#endif

    VirtualClock clock;
    Application::pointer app =
        Application::create(clock, getTestConfig(0, mode));
    app->start();

    LedgerDelta delta(app->getLedgerManager().getCurrentLedgerHeader());
    auto& db = app->getDatabase();
    auto le = EntryFrame::FromXDR(validLedgerEntryGenerator(3));
    auto ctx = db.captureAndLogSQL("ledger-insert");
    le->storeAddOrChange(delta, db);
}
