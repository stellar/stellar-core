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
#include "transactions/TxTests.h"

#include "main/Config.h"

using namespace stellar;
using namespace std;

typedef std::unique_ptr<Application> appPtr;

TEST_CASE("ledger performance test", "[ledger][performance]")
{

    Config cfg(getTestConfig());

    cfg.DATABASE = "sqlite3://test.db";

    cfg.REBUILD_DB = true;
    VirtualClock clock;
    Application::pointer app = Application::create(clock, cfg);
    app->start();

            
    int seq = 1;
    LOG(INFO) << "Signing #";
    TxSetFramePtr txSet = make_shared<TxSetFrame>(app->getLedgerMaster().getLastClosedLedgerHeader().hash);
    for (int iTx = 0; iTx < 1000; iTx++)
    {
        auto accountName = "Account-" + to_string(txSet->size());
        auto from = txtest::getRoot();
        auto to =  txtest::getAccount(accountName.c_str());
        txSet->add(txtest::createPaymentTx(from, to, seq++, 20000000));
    }

    for (int i = 1; i < 10; i++)
    {

        LOG(INFO) << "Closing #" << i;
        LedgerCloseData ledgerData(i, txSet, 1, 10);
        app->getLedgerMaster().closeLedger(ledgerData);
    }


}
