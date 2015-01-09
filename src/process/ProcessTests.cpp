// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "main/Config.h"
#include "main/Application.h"
#include "xdrpp/autocheck.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include "util/Timer.h"
#include <future>
#include "process/ProcessGateway.h"

using namespace stellar;

TEST_CASE("subprocess", "[process]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    Application app(clock, cfg);
    auto evt = app.getProcessGateway().runProcess("hostname");
    bool exited = false;
    evt.async_wait([&](asio::error_code ec)
                   {
                       LOG(DEBUG) << "process exited: " << ec;
                       if (ec)
                           LOG(DEBUG) << "error code: " << ec.message();
                       exited = true;
                   });

    while (!exited && !app.getMainIOService().stopped())
    {
        app.getMainIOService().poll_one();
    }
}
