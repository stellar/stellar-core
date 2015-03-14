// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "util/Timer.h"
#include "main/Application.h"
#include "main/test.h"
#include "main/Config.h"
#include "lib/catch.hpp"
#include "ledger/LedgerDelta.h"
#include "ledger/LedgerMaster.h"
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

            clampLow(0, le.offer().price.n);
            clampLow(1, le.offer().price.d);
        }
        break;

        case ACCOUNT:
            break;
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
    LedgerDelta delta(app->getLedgerMaster().getCurrentLedgerHeader());
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
