#include "util/asio.h"
#include <cassert>
#include "main/Application.h"
#include "main/Config.h"

#include <cassert>
#include "util/make_unique.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "overlay/PeerMaster.h"
#include "util/Timer.h"
#include "database/Database.h"
#include <soci.h>

using namespace stellar;
using namespace std;
using namespace soci;

void PeerMaster::unitTest_addPeerList()
{
    vector<string> peers10 { "127.0.0.1:2011", "127.0.0.1:2012", "127.0.0.1:2013", "127.0.0.1:2014" };
    addPeerList(peers10, 10);

    vector<string> peers3 { "127.0.0.1:201", "127.0.0.1:202", "127.0.0.1:203", "127.0.0.1:204" };
    addPeerList(peers3, 3);
}

TEST_CASE("addPeerList() adds", "[peer]") {
    Config cfg(getTestConfig());
    cfg.DATABASE = "sqlite3://stellar.db";
    cfg.RUN_STANDALONE = true;
    VirtualClock clock;
    Application app(clock, cfg);
    app.start();

    app.getOverlayGateway().unitTest_addPeerList();

    soci::session mSession;
    mSession.open(app.getConfig().DATABASE);

    rowset<row> rs = mSession.prepare << "SELECT ip,port from Peers order by rank limit 5 ";
    vector<string> actual;
    for (auto it = rs.begin(); it != rs.end(); ++it)
        actual.push_back(it->get<string>(0) + ":" + to_string(it->get<int>(1)));

    vector<string> expected{ "127.0.0.1:201", "127.0.0.1:202", "127.0.0.1:203", "127.0.0.1:204", "127.0.0.1:2011" };
    REQUIRE(actual == expected);
}

