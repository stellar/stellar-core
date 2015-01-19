// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "main/Application.h"
#include "util/Timer.h"
#include "util/make_unique.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include "crypto/Base58.h"
#include "ledger/LedgerMaster.h"

#include "main/Config.h"

using namespace stellar;
using namespace std;

typedef std::unique_ptr<Application> appPtr;

TEST_CASE("ledgerheader", "[ledger]")
{

    Config cfg(getTestConfig());

    cfg.DATABASE = "sqlite3://test.db";

    VirtualClock clock;
    Application app(clock, cfg);
    app.start();

    TxSetFramePtr txSet = make_shared<TxSetFrame>();

    // close this ledger
    app.getLedgerMaster().closeLedger(txSet);

    SECTION("load existing ledger")
    {
        Config cfg2(cfg);
        cfg2.START_NEW_NETWORK = false;
        VirtualClock clock2;
        Application app2(clock2, cfg2);
        app2.start();

        REQUIRE(app.getLedgerMaster().getLastClosedLedgerHeader().hash ==
            app2.getLedgerMaster().getLastClosedLedgerHeader().hash);
    }

}
