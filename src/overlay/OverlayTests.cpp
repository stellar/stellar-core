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

TEST_CASE("loopback peer hello", "[overlay]")
{
    Config const& cfg1 = getTestConfig(0);
    VirtualClock clock;
    std::vector<Application::pointer> apps;
    apps.push_back(Application::create(clock, cfg1));

    Config const& cfg2 = getTestConfig(1);
    apps.push_back(Application::create(clock, cfg2));

    LoopbackPeerConnection conn(*apps[0], *apps[1]);

    size_t i = 0;
    auto& io = clock.getIOService();
    while (!io.stopped())
    {
        for (auto app : apps)
        {
            io.poll_one();
            if (++i > 100)
                io.stop();
        }
    }
}
