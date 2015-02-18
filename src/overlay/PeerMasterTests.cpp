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
#include "util/TmpDir.h"
#include <soci.h>

using namespace stellar;
using namespace std;
using namespace soci;

namespace stellar 
{
class PeerMasterTests 
{
protected:
    VirtualClock clock;
    Application app{ clock, getTestConfig() };
    PeerMaster peerMaster{ app };


    void test_addPeerList()
    {
        vector<string> peers10{ "127.0.0.1:2011", "127.0.0.1:2012", "127.0.0.1:2013", "127.0.0.1:2014" };
        peerMaster.addPeerList(peers10, 10);

        vector<string> peers3{ "127.0.0.1:201", "127.0.0.1:202", "127.0.0.1:203", "127.0.0.1:204" };
        peerMaster.addPeerList(peers3, 3);

        rowset<row> rs = app.getDatabase().getSession().prepare << "SELECT ip,port from Peers order by rank limit 5 ";
        vector<string> actual;
        for (auto it = rs.begin(); it != rs.end(); ++it)
            actual.push_back(it->get<string>(0) + ":" + to_string(it->get<int>(1)));

        vector<string> expected{ "127.0.0.1:201", "127.0.0.1:202", "127.0.0.1:203", "127.0.0.1:204", "127.0.0.1:2011" };
        REQUIRE(actual == expected);
    }
};

TEST_CASE_METHOD(PeerMasterTests, "addPeerList() adds", "[peer]") 
{
    test_addPeerList();
}

}