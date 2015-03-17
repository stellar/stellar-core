// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "util/asio.h"
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
    Application::pointer app = Application::create(clock, cfg);
    auto evt = app->getProcessGateway().runProcess("hostname");
    bool exited = false;
    bool failed = false;
    evt.async_wait([&](asio::error_code ec)
                   {
                       CLOG(DEBUG, "Process") << "process exited: " << ec;
                       if (ec)
                       {
                           CLOG(DEBUG, "Process")
                               << "error code: " << ec.message();
                       }
                       failed = !!ec;
                       exited = true;
                   });

    while (!exited && !clock.getIOService().stopped())
    {
        clock.crank(false);
    }
    REQUIRE(!failed);
}

TEST_CASE("subprocess fails", "[process]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    Application::pointer app = Application::create(clock, cfg);
    auto evt = app->getProcessGateway().runProcess("hostname -xsomeinvalid");
    bool exited = false;
    bool failed = false;
    evt.async_wait([&](asio::error_code ec)
                   {
                       CLOG(DEBUG, "Process") << "process exited: " << ec;
                       if (ec)
                       {
                           CLOG(DEBUG, "Process")
                               << "error code: " << ec.message();
                       }
                       failed = !!ec;
                       exited = true;
                   });

    while (!exited && !clock.getIOService().stopped())
    {
        clock.crank(false);
    }
    REQUIRE(failed);
}

TEST_CASE("subprocess redirect to file", "[process]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    Application::pointer appPtr = Application::create(clock, cfg);
    Application& app = *appPtr;
    std::string filename("hostname.txt");
    auto evt = app.getProcessGateway().runProcess("hostname", filename);
    bool exited = false;
    evt.async_wait([&](asio::error_code ec)
                   {
                       CLOG(DEBUG, "Process") << "process exited: " << ec;
                       if (ec)
                       {
                           CLOG(DEBUG, "Process")
                               << "error code: " << ec.message();
                       }
                       exited = true;
                   });

    while (!exited && !clock.getIOService().stopped())
    {
        clock.crank(false);
    }

    std::ifstream in(filename);
    CHECK(in);
    std::string s;
    in >> s;
    CLOG(DEBUG, "Process") << "opened redirect file, read: " << s;
    CHECK(!s.empty());
    std::remove(filename.c_str());
}
