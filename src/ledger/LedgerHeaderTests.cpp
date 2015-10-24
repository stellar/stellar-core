// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Application.h"
#include "util/Timer.h"
#include "util/make_unique.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include "ledger/LedgerManager.h"
#include "herder/LedgerCloseData.h"
#include "xdrpp/marshal.h"

#include "main/Config.h"

using namespace stellar;
using namespace std;

typedef std::unique_ptr<Application> appPtr;

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
        Application::pointer app2 = Application::create(clock2, cfg2,false);
        app2->start();

        REQUIRE(saved ==
                app2->getLedgerManager().getLastClosedLedgerHeader().hash);
    }

    SECTION("update")
    {
        VirtualClock clock;
        Application::pointer app = Application::create(clock, cfg);
        app->start();

        auto const& lcl = app->getLedgerManager().getLastClosedLedgerHeader();
        auto const& lastHash = lcl.hash;
        TxSetFramePtr txSet = make_shared<TxSetFrame>(lastHash);

        REQUIRE(lcl.header.baseFee == 100);
        REQUIRE(lcl.header.maxTxSetSize == 100);

        SECTION("fee")
        {
            StellarValue sv(txSet->getContentsHash(), 2, emptyUpgradeSteps, 0);
            {
                LedgerUpgrade up(LEDGER_UPGRADE_BASE_FEE);
                up.newBaseFee() = 1000;
                Value v(xdr::xdr_to_opaque(up));
                sv.upgrades.emplace_back(v.begin(), v.end());
            }

            LedgerCloseData ledgerData(lcl.header.ledgerSeq + 1, txSet, sv);
            app->getLedgerManager().closeLedger(ledgerData);

            auto& newLCL = app->getLedgerManager().getLastClosedLedgerHeader();

            REQUIRE(newLCL.header.baseFee == 1000);
        }
        SECTION("max tx")
        {
            StellarValue sv(txSet->getContentsHash(), 2, emptyUpgradeSteps, 0);
            {
                LedgerUpgrade up(LEDGER_UPGRADE_MAX_TX_SET_SIZE);
                up.newMaxTxSetSize() = 1300;
                Value v(xdr::xdr_to_opaque(up));
                sv.upgrades.emplace_back(v.begin(), v.end());
            }

            LedgerCloseData ledgerData(lcl.header.ledgerSeq + 1, txSet, sv);
            app->getLedgerManager().closeLedger(ledgerData);

            auto& newLCL = app->getLedgerManager().getLastClosedLedgerHeader();

            REQUIRE(newLCL.header.maxTxSetSize == 1300);
        }
    }
}
