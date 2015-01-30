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
#include "crypto/SecretKey.h"
#include "main/Config.h"

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
    Config& cfg1 = getTestConfig();
    VirtualClock clock;
    std::vector<appPtr> apps;
    apps.emplace_back(stellar::make_unique<Application>(clock, cfg1));

    Config cfg2;
    cfg2.LOG_FILE_PATH = cfg1.LOG_FILE_PATH;
    cfg2.RUN_STANDALONE = true;
    cfg2.START_NEW_NETWORK = true;
    apps.emplace_back(stellar::make_unique<Application>(clock, cfg2));

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
