// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/Config.h"
#include "process/ProcessManager.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Fs.h"
#include "util/Logging.h"
#include "util/Timer.h"
#include "util/TmpDir.h"
#include "xdrpp/autocheck.h"
#include <chrono>
#include <fmt/format.h>
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
    auto evt = app->getProcessManager().runProcess("hostname", "").lock();
    REQUIRE(evt);
    evt->async_wait([&](asio::error_code ec) {
        CLOG_DEBUG(Process, "process exited: {}", ec);
        if (ec)
        {
            CLOG_DEBUG(Process, "error code: {}", ec.message());
        }
        failed = !!ec;
        exited = true;
    });

    while (!exited && !clock.getIOContext().stopped())
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
    auto evt = app->getProcessManager()
                   .runProcess("hostname -xsomeinvalid", "")
                   .lock();
    REQUIRE(evt);
    evt->async_wait([&](asio::error_code ec) {
        CLOG_DEBUG(Process, "process exited: {}", ec);
        if (ec)
        {
            CLOG_DEBUG(Process, "error code: {}", ec.message());
        }
        failed = !!ec;
        exited = true;
    });

    while (!exited && !clock.getIOContext().stopped())
    {
        clock.crank(true);
    }
    REQUIRE(failed);
}

TEST_CASE("subprocess redirect to new file", "[process]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    Application::pointer appPtr = createTestApplication(clock, cfg);
    Application& app = *appPtr;
    TmpDir tmpDir = app.getTmpDirManager().tmpDir("subprocess-redirect");
    std::string filename(fmt::format("{}/hostname.txt", tmpDir.getName()));
    bool exited = false;
    auto evt = app.getProcessManager().runProcess("hostname", filename).lock();
    REQUIRE(evt);
    evt->async_wait([&](asio::error_code ec) {
        CLOG_DEBUG(Process, "process exited: {}", ec);
        if (ec)
        {
            CLOG_DEBUG(Process, "error code: {}", ec.message());
        }
        exited = true;
    });

    while (!exited && !clock.getIOContext().stopped())
    {
        clock.crank(true);
    }

    std::ifstream in(filename);
    CHECK(in);
    in.exceptions(std::ios::badbit);
    std::string s;
    in >> s;
    CLOG_DEBUG(Process, "opened redirect file, read: {}", s);
    CHECK(!s.empty());
}

TEST_CASE("subprocess redirect to existing file", "[process]")
{
    // This test should have the process fail, because there's already
    // an existing file.

    VirtualClock clock;
    Config const& cfg = getTestConfig();
    Application::pointer appPtr = createTestApplication(clock, cfg);
    Application& app = *appPtr;
    TmpDir tmpDir = app.getTmpDirManager().tmpDir("subprocess-redirect");
    std::string filename(fmt::format("{}/hostname.txt", tmpDir.getName()));
    std::string data = "12345hello54321";
    {
        std::ofstream tout(filename);
        tout << data;
    }

    auto evt = app.getProcessManager().runProcess("hostname", filename).lock();
    REQUIRE(!evt);
    std::ifstream in(filename);
    CHECK(in);
    in.exceptions(std::ios::badbit);
    std::string s;
    in >> s;
    CLOG_DEBUG(Process, "opened redirect file, read: {}", s);
    CHECK(s == data);
}

TEST_CASE("subprocess storm", "[process]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    Application::pointer appPtr = createTestApplication(clock, cfg);
    Application& app = *appPtr;
    TmpDir tmpDir = app.getTmpDirManager().tmpDir("process-storm");

    size_t n = 100;
    size_t completed = 0;

    std::string dir = tmpDir.getName();
    fs::mkpath(dir + "/src");
    fs::mkpath(dir + "/dst");

    for (size_t i = 0; i < n; ++i)
    {
        std::string src(fmt::format("{:s}/src/{:d}", dir, i));
        std::string dst(fmt::format("{:s}/dst/{:d}", dir, i));
        CLOG_INFO(Process, "making file {}", src);
        {
            std::ofstream out;
            out.exceptions(std::ios::failbit | std::ios::badbit);
            out.open(src);
            out << i;
        }
        auto evt = app.getProcessManager()
                       .runProcess("mv " + src + " " + dst, "")
                       .lock();
        REQUIRE(evt);
        evt->async_wait([&](asio::error_code ec) {
            CLOG_INFO(Process, "process exited: {}", ec);
            if (ec)
            {
                CLOG_DEBUG(Process, "error code: {}", ec.message());
            }
            ++completed;
        });
    }

    size_t last = 0;
    while (completed < n && !clock.getIOContext().stopped())
    {
        clock.crank(false);
        size_t n2 = app.getProcessManager().getNumRunningProcesses();
        if (last != n2)
        {
            CLOG_INFO(Process, "running subprocess count: {}", n2);
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
    VirtualClock clock1;
    VirtualClock clock2;

    auto const& cfg1 = getTestConfig(0);
    auto const& cfg2 = getTestConfig(1);
    auto app1 = createTestApplication(clock1, cfg1);
    auto app2 = createTestApplication(clock2, cfg2);
#ifdef _WIN32
    std::string command = "waitfor /T 10 pause";
#else
    std::string command = "sleep 10";
#endif
    std::vector<asio::error_code> errorCodes;
    size_t exitedCount = 0;
    std::vector<std::shared_ptr<ProcessExitEvent>> events = {
        app1->getProcessManager().runProcess(command, "").lock(),
        app2->getProcessManager().runProcess(command, "").lock()};
    for (auto const& event : events)
    {
        REQUIRE(event);
        event->async_wait([&](asio::error_code ec) {
            CLOG_DEBUG(Process, "process exited: {}", ec);
            if (ec)
            {
                CLOG_DEBUG(Process, "error code: {}", ec.message());
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

    while (exitedCount < events.size() && !clock1.getIOContext().stopped() &&
           !clock2.getIOContext().stopped())
    {
        clock1.crank(true);
        clock2.crank(true);
    }
    REQUIRE(exitedCount == events.size());
    for (auto const& errorCode : errorCodes)
    {
        REQUIRE(errorCode == asio::error::operation_aborted);
    }
}

TEST_CASE("ProcessManager::tryProcessShutdown test", "[process]")
{
    VirtualClock clock;
    auto const& cfg = getTestConfig(0);
    auto app = createTestApplication(clock, cfg);

#ifdef _WIN32
    std::string command = "waitfor /T 10 pause";
#else
    std::string command = "sleep 10";
#endif
    std::shared_ptr<ProcessExitEvent> event =
        app->getProcessManager().runProcess(command, "").lock();

    asio::error_code errorCode;
    bool exited = false;

    REQUIRE(event);
    event->async_wait([&](asio::error_code ec) {
        CLOG_DEBUG(Process, "process exited: {}", ec);
        if (ec)
        {
            CLOG_DEBUG(Process, "error code: {}", ec.message());
        }
        errorCode = ec;
        exited = true;
    });

    // Wait, just in case the processes haven't started yet
    std::this_thread::sleep_for(std::chrono::seconds(1));

    app->getProcessManager().tryProcessShutdown(event);
    while (!exited && !clock.getIOContext().stopped())
    {
        clock.crank(true);
    }
    REQUIRE(errorCode == asio::error::operation_aborted);
}
