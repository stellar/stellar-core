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
#include <chrono>
#include <future>
#include <thread>

using namespace stellar;

TEST_CASE("subprocess", "[process]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    Application::pointer app = createTestApplication(clock, cfg);
    bool exited = false;
    bool failed = false;
    auto evt = app->getProcessManager().runProcess("hostname");
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
    bool exited = false;
    bool failed = false;
    auto evt = app->getProcessManager().runProcess("hostname -xsomeinvalid");
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
    bool exited = false;
    auto evt = app.getProcessManager().runProcess("hostname", filename);
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

TEST_CASE("shutdown while process running", "[process]")
{
    VirtualClock clock;
    auto const& cfg1 = getTestConfig(0);
    auto const& cfg2 = getTestConfig(1);
    auto app1 = createTestApplication(clock, cfg1);
    auto app2 = createTestApplication(clock, cfg2);
#ifdef _WIN32
    std::string command = "waitfor /T 10 pause";
#else
    std::string command = "sleep 10";
#endif
    std::vector<asio::error_code> errorCodes;
    size_t exitedCount = 0;
    std::vector<ProcessExitEvent> events = {
        app1->getProcessManager().runProcess(command),
        app2->getProcessManager().runProcess(command)};
    for (auto& event : events)
    {
        event.async_wait([&](asio::error_code ec) {
            CLOG(DEBUG, "Process") << "process exited: " << ec;
            if (ec)
            {
                CLOG(DEBUG, "Process") << "error code: " << ec.message();
            }
            exitedCount++;
            errorCodes.push_back(ec);
        });
    }

    // Wait, just in case the processes haven't started yet
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // Shutdown so we force the command execution to fail
    app1->getProcessManager().shutdown();
    app2->getProcessManager().shutdown();

    while (exitedCount < events.size() && !clock.getIOService().stopped())
    {
        clock.crank(true);
    }
    REQUIRE(exitedCount == events.size());
    for (auto const& errorCode : errorCodes)
    {
        REQUIRE(errorCode == asio::error::operation_aborted);
    }
}
