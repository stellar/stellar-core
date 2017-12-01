// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "crypto/Hex.h"
#include "herder/LedgerCloseData.h"
#include "ledger/LedgerManager.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Logging.h"
#include "util/Timer.h"
#include "util/make_unique.h"
#include "xdrpp/marshal.h"

#include "main/Config.h"

using namespace stellar;
using namespace std;

typedef std::unique_ptr<Application> appPtr;

TEST_CASE("genesisledger", "[ledger]")
{
    VirtualClock clock{};
    auto cfg = getTestConfig(0);
    auto app = Application::create<ApplicationImpl>(clock, cfg);
    app->start();

    auto const& lcl = app->getLedgerManager().getLastClosedLedgerHeader();
    auto const& header = lcl.header;
    REQUIRE(header.ledgerVersion == 0);
    REQUIRE(header.previousLedgerHash == Hash{});
    REQUIRE(header.scpValue.txSetHash == Hash{});
    REQUIRE(header.scpValue.closeTime == 0);
    REQUIRE(header.scpValue.upgrades.size() == 0);
    REQUIRE(header.txSetResultHash == Hash{});
    REQUIRE(binToHex(header.bucketListHash) ==
            "4e6a8404d33b17eee7031af0b3606b6af8e36fe5a3bff59e4e5e420bd0ad3bf4");
    REQUIRE(header.ledgerSeq == 1);
    REQUIRE(header.totalCoins == 1000000000000000000);
    REQUIRE(header.feePool == 0);
    REQUIRE(header.inflationSeq == 0);
    REQUIRE(header.idPool == 0);
    REQUIRE(header.baseFee == 100);
    REQUIRE(header.baseReserve == 100000000);
    REQUIRE(header.maxTxSetSize == 100);
    REQUIRE(header.skipList.size() == 4);
    REQUIRE(header.skipList[0] == Hash{});
    REQUIRE(header.skipList[1] == Hash{});
    REQUIRE(header.skipList[2] == Hash{});
    REQUIRE(header.skipList[3] == Hash{});
    REQUIRE(binToHex(lcl.hash) ==
            "caf73c70dde8134f792535756cc3212f65007883e8959adf92e48062f401e543");
}

TEST_CASE("ledgerheader", "[ledger]")
{
    Config cfg(getTestConfig(0, Config::TESTDB_ON_DISK_SQLITE));

    Hash saved;
    {
        VirtualClock clock;
        Application::pointer app = Application::create(clock, cfg);
        app->start();

        auto const& lcl = app->getLedgerManager().getLastClosedLedgerHeader();
        auto const& lastHash = lcl.hash;
        TxSetFramePtr txSet = make_shared<TxSetFrame>(lastHash);

        // close this ledger
        StellarValue sv(txSet->getContentsHash(), 1, emptyUpgradeSteps, 0);
        LedgerCloseData ledgerData(lcl.header.ledgerSeq + 1, txSet, sv);
        app->getLedgerManager().closeLedger(ledgerData);

        saved = app->getLedgerManager().getLastClosedLedgerHeader().hash;
    }

    SECTION("load existing ledger")
    {
        Config cfg2(cfg);
        cfg2.FORCE_SCP = false;
        VirtualClock clock2;
        Application::pointer app2 = Application::create(clock2, cfg2, false);
        app2->start();

        REQUIRE(saved ==
                app2->getLedgerManager().getLastClosedLedgerHeader().hash);
    }
}

TEST_CASE("base reserve", "[ledger]")
{
    Config const& cfg = getTestConfig();

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);

    app->start();

    auto const& lcl = app->getLedgerManager().getLastClosedLedgerHeader();
    REQUIRE(lcl.header.baseReserve == 100000000);
    const uint32 n = 20000;
    int64 expectedReserve = 2000200000000ll;

    for_versions_to(8, *app, [&]() {
        REQUIRE(app->getLedgerManager().getMinBalance(n) < expectedReserve);
    });
    for_versions_from(9, *app, [&]() {
        REQUIRE(app->getLedgerManager().getMinBalance(n) == expectedReserve);
    });
}
