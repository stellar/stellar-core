// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "main/Application.h"
#include "overlay/LoopbackPeer.h"
#include "util/make_unique.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include "lib/json/json.h"
#include "TxTests.h"

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

    // set up world
    SecretKey root = getRoot(app.getNetworkID());
    SecretKey gateway = getAccount("gw");

    SequenceNumber rootSeq = getAccountSeqNum(root, app) + 1;

    const int64_t minBalance = app.getLedgerManager().getMinBalance(3)-100;

    applyCreateAccountTx(app, root, gateway, rootSeq++, minBalance);
    SequenceNumber gateway_seq = getAccountSeqNum(gateway, app) + 1;

    DataValue value,value2;
    value.resize(64);
    value2.resize(64);
    for(int n = 0; n < 64; n++)
    {
        value[n] = (unsigned char) n;
        value2[n] = (unsigned char) n + 3;
    }

    std::string t1("test");
    std::string t2("test2");
    std::string t3("test3");
    std::string t4("test4");

    applyManageData(app, gateway, t1, &value, gateway_seq++);
    applyManageData(app, gateway, t2, &value, gateway_seq++);
    // try to add too much data
    applyManageData(app, gateway, t3, &value, gateway_seq++, MANAGE_DATA_LOW_RESERVE);

    // modify an existing data entry
    applyManageData(app, gateway, t1, &value2, gateway_seq++);

    // clear an existing data entry
    applyManageData(app, gateway, t1, nullptr, gateway_seq++);
    
    // can now add test3 since test was removed
    applyManageData(app, gateway, t3, &value, gateway_seq++);

    // fail to remove data entry that isn't present
    applyManageData(app, gateway, t4, nullptr, gateway_seq++, MANAGE_DATA_NAME_NOT_FOUND);

}
