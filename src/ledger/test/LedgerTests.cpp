// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "main/Application.h"
#include "test/test.h"

#include <lib/catch.hpp>

using namespace stellar;

TEST_CASE("cannot close ledger with unsupported ledger version", "[ledger]")
{
    VirtualClock clock;
    Application::pointer app = Application::create(clock, getTestConfig(0));
    app->start();

    auto applyEmptyLedger = [&]() {
        auto const& lcl = app->getLedgerManager().getLastClosedLedgerHeader();
        auto txSet = std::make_shared<TxSetFrame>(lcl.hash);

        StellarValue sv(txSet->getContentsHash(), 1, emptyUpgradeSteps,
                        STELLAR_VALUE_BASIC);
        LedgerCloseData ledgerData(lcl.header.ledgerSeq + 1, txSet, sv);
        app->getLedgerManager().closeLedger(ledgerData);
    };

    applyEmptyLedger();
    {
        LedgerTxn ltx(app->getLedgerTxnRoot());
        ltx.loadHeader().current().ledgerVersion =
            Config::CURRENT_LEDGER_PROTOCOL_VERSION;
        ltx.commit();
    }
    applyEmptyLedger();
    {
        LedgerTxn ltx(app->getLedgerTxnRoot());
        ltx.loadHeader().current().ledgerVersion =
            Config::CURRENT_LEDGER_PROTOCOL_VERSION + 1;
        ltx.commit();
    }
    REQUIRE_THROWS_AS(applyEmptyLedger(), std::runtime_error);
}
