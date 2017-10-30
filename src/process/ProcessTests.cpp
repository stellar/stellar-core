// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "lib/catch.hpp"
#include "lib/util/format.h"
#include "main/Application.h"
#include "main/Config.h"
#include "process/ProcessManager.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Fs.h"
#include "util/Logging.h"
#include "util/Timer.h"
#include "xdrpp/autocheck.h"
#include <future>

using namespace stellar;

TEST_CASE("subprocess", "[process]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    Application::pointer app = createTestApplication(clock, cfg);
    auto evt = app->getProcessManager().runProcess("hostname");
    bool exited = false;
    bool failed = false;
    evt.async_wait([&](asio::error_code ec) {
        CLOG(DEBUG, "Process") << "process exited: " << ec;
        if (ec)
        {
            CLOG(DEBUG, "Process") << "error code: " << ec.message();
        }
        failed = !!ec;
        exited = true;
    });

    while (!exited && !clock.getIOService().stopped())
    {
        clock.crank(true);
    }
    REQUIRE(!failed);
}

TEST_CASE("subprocess fails", "[process]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    Application::pointer app = createTestApplication(clock, cfg);
    auto evt = app->getProcessManager().runProcess("hostname -xsomeinvalid");
    bool exited = false;
    bool failed = false;
    evt.async_wait([&](asio::error_code ec) {
        CLOG(DEBUG, "Process") << "process exited: " << ec;
        if (ec)
        {
            CLOG(DEBUG, "Process") << "error code: " << ec.message();
        }
        failed = !!ec;
        exited = true;
    });

    while (!exited && !clock.getIOService().stopped())
    {
        clock.crank(true);
    }
    REQUIRE(failed);
}

TEST_CASE("subprocess redirect to file", "[process]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    Application::pointer appPtr = createTestApplication(clock, cfg);
    Application& app = *appPtr;
    std::string filename("hostname.txt");
    auto evt = app.getProcessManager().runProcess("hostname", filename);
    bool exited = false;
    evt.async_wait([&](asio::error_code ec) {
        CLOG(DEBUG, "Process") << "process exited: " << ec;
        if (ec)
        {
            CLOG(DEBUG, "Process") << "error code: " << ec.message();
        }
        exited = true;
    });

    while (!exited && !clock.getIOService().stopped())
    {
        clock.crank(true);
    }

    std::ifstream in(filename);
    CHECK(in);
    std::string s;
    in >> s;
    CLOG(DEBUG, "Process") << "opened redirect file, read: " << s;
    CHECK(!s.empty());
    std::remove(filename.c_str());
}

TEST_CASE("subprocess storm", "[process]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    Application::pointer appPtr = createTestApplication(clock, cfg);
    Application& app = *appPtr;

    size_t n = 100;
    size_t completed = 0;

    std::string dir(cfg.BUCKET_DIR_PATH + "/tmp/process-storm");
    fs::mkdir(dir);
    fs::mkpath(dir + "/src");
    fs::mkpath(dir + "/dst");

    for (size_t i = 0; i < n; ++i)
    {
        std::string src(fmt::format("{:s}/src/{:d}", dir, i));
        std::string dst(fmt::format("{:s}/dst/{:d}", dir, i));
        CLOG(INFO, "Process") << "making file " << src;
        {
            std::ofstream out(src);
            out << i;
        }
        auto evt = app.getProcessManager().runProcess("mv " + src + " " + dst);
        evt.async_wait([&](asio::error_code ec) {
            CLOG(INFO, "Process") << "process exited: " << ec;
            if (ec)
            {
                CLOG(DEBUG, "Process") << "error code: " << ec.message();
            }
            ++completed;
        });
    }

    size_t last = 0;
    while (completed < n && !clock.getIOService().stopped())
    {
        clock.crank(false);
        size_t n2 = app.getProcessManager().getNumRunningProcesses();
        if (last != n2)
        {
            CLOG(INFO, "Process") << "running subprocess count: " << n2;
        }
        last = n2;
        REQUIRE(n2 <= cfg.MAX_CONCURRENT_SUBPROCESSES);
    }

    for (size_t i = 0; i < n; ++i)
    {
        std::string src(fmt::format("{:s}/src/{:d}", dir, i));
        std::string dst(fmt::format("{:s}/dst/{:d}", dir, i));
        REQUIRE(!fs::exists(src));
        REQUIRE(fs::exists(dst));
    }
}
