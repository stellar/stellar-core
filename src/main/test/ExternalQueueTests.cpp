// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/CommandHandler.h"
#include "main/Config.h"
#include "main/ExternalQueue.h"
#include "simulation/Simulation.h"
#include "test/TestUtils.h"
#include "test/test.h"

using namespace stellar;

TEST_CASE("cursors", "[externalqueue]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    Application::pointer app = createTestApplication(clock, cfg);

    app->start();

    ExternalQueue ps(*app);
    std::map<std::string, uint32> curMap;
    app->getCommandHandler().manualCmd("setcursor?id=FOO&cursor=123");
    app->getCommandHandler().manualCmd("setcursor?id=BAR&cursor=456");

    SECTION("get non-existent cursor")
    {
        ps.getCursorForResource("NONEXISTENT", curMap);
        REQUIRE(curMap.size() == 0);
    }

    SECTION("get single cursor")
    {
        ps.getCursorForResource("FOO", curMap);
        REQUIRE(curMap.size() == 1);
    }

    SECTION("get all cursors")
    {
        ps.getCursorForResource("", curMap);
        REQUIRE(curMap.size() == 2);
    }
}
