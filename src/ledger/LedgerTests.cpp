// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "LedgerTestUtils.h"
#include "database/Database.h"
#include "herder/LedgerCloseData.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerState.h"
#include "ledger/LedgerStateEntry.h"
#include "ledger/LedgerStateHeader.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/Config.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Logging.h"
#include "util/Timer.h"
#include "util/types.h"
#include <xdrpp/autocheck.h>

using namespace stellar;

TEST_CASE("cannot close ledger with unsupported ledger version", "[ledger]")
{
    VirtualClock clock;
    Application::pointer app = Application::create(clock, getTestConfig(0));
    app->start();

    auto applyEmptyLedger = [&]() {
        auto const& lcl = app->getLedgerManager().getLastClosedLedgerHeader();
        auto txSet = std::make_shared<TxSetFrame>(lcl.hash);

        StellarValue sv(txSet->getContentsHash(), 1, emptyUpgradeSteps, 0);
        LedgerCloseData ledgerData(lcl.header.ledgerSeq + 1, txSet, sv);
        app->getLedgerManager().closeLedger(ledgerData);
    };

    applyEmptyLedger();
    {
        LedgerState ls(app->getLedgerStateRoot());
        ls.loadHeader().current().ledgerVersion =
            Config::CURRENT_LEDGER_PROTOCOL_VERSION;
        ls.commit();
    }
    applyEmptyLedger();
    {
        LedgerState ls(app->getLedgerStateRoot());
        ls.loadHeader().current().ledgerVersion =
            Config::CURRENT_LEDGER_PROTOCOL_VERSION + 1;
        ls.commit();
    }
    REQUIRE_THROWS_AS(applyEmptyLedger(), std::runtime_error);
}
