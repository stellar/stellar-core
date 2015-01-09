// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "main/Application.h"
#include "overlay/LoopbackPeer.h"
#include "util/make_unique.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include "util/Timer.h"

using namespace stellar;

typedef std::unique_ptr<Application> appPtr;

static bool
allStopped(std::vector<appPtr>& apps)
{
    for (appPtr& app : apps)
    {
        if (!app->getMainIOService().stopped())
        {
            return false;
        }
    }
    return true;
}

TEST_CASE("loopback peer hello", "[overlay]")
{

    Config const& cfg = getTestConfig();
    VirtualClock clock;
    std::vector<appPtr> apps;
    apps.emplace_back(stellar::make_unique<Application>(clock, cfg));
    apps.emplace_back(stellar::make_unique<Application>(clock, cfg));

    LoopbackPeerConnection conn(*apps[0], *apps[1]);

    size_t i = 0;
    while (!allStopped(apps))
    {
        for (appPtr& app : apps)
        {
            app->getMainIOService().poll_one();
            if (++i > 100)
                app->getMainIOService().stop();
        }
    }
}
