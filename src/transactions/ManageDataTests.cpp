// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "lib/catch.hpp"
#include "lib/json/json.h"
#include "main/Application.h"
#include "overlay/LoopbackPeer.h"
#include "test/TestAccount.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "util/Logging.h"
#include "util/make_unique.h"
#include "xdrpp/marshal.h"

using namespace stellar;
using namespace stellar::txtest;

typedef std::unique_ptr<Application> appPtr;

// add data
// change data
// remove data
// remove data that isn't there
// add too much data
TEST_CASE("manage data", "[tx][managedata]")
{
    Config const& cfg = getTestConfig();

    VirtualClock clock;
    Application::pointer appPtr = Application::create(clock, cfg);
    Application& app = *appPtr;

    app.start();
    upgradeToCurrentLedgerVersion(app);

    // set up world
    auto root = TestAccount::createRoot(app);

    const int64_t minBalance = app.getLedgerManager().getMinBalance(3) - 100;

    auto gateway = root.create("gw", minBalance);

    DataValue value, value2;
    value.resize(64);
    value2.resize(64);
    for (int n = 0; n < 64; n++)
    {
        value[n] = (unsigned char)n;
        value2[n] = (unsigned char)n + 3;
    }

    std::string t1("test");
    std::string t2("test2");
    std::string t3("test3");
    std::string t4("test4");

    applyManageData(app, gateway, t1, &value, gateway.nextSequenceNumber());
    applyManageData(app, gateway, t2, &value, gateway.nextSequenceNumber());
    // try to add too much data
    applyManageData(app, gateway, t3, &value, gateway.nextSequenceNumber(),
                    MANAGE_DATA_LOW_RESERVE);

    // modify an existing data entry
    applyManageData(app, gateway, t1, &value2, gateway.nextSequenceNumber());

    // clear an existing data entry
    applyManageData(app, gateway, t1, nullptr, gateway.nextSequenceNumber());

    // can now add test3 since test was removed
    applyManageData(app, gateway, t3, &value, gateway.nextSequenceNumber());

    // fail to remove data entry that isn't present
    applyManageData(app, gateway, t4, nullptr, gateway.nextSequenceNumber(),
                    MANAGE_DATA_NAME_NOT_FOUND);
}
