// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#ifndef ASIO_SEPARATE_COMPILATION
#define ASIO_SEPARATE_COMPILATION
#endif
#include <asio.hpp>

#define STELLARD_REAL_TIMER_FOR_CERTAIN_NOT_JUST_VIRTUAL_TIME
#include "util/Timer.h"

#include "main/Application.h"
#include "util/Logging.h"
#include "process/ProcessGateway.h"
#include "process/ProcessMaster.h"
#include <string>
#include <functional>

namespace stellar
{

#ifdef _MSC_VER
#include <windows.h>
#include <tchar.h>

ProcessMaster::ProcessMaster(Application& app)
    : mApp(app), mSigChild(app.getMainIOService())
{
}

void
ProcessMaster::startSignalWait()
{
    // No-op on windows, uses waitable object handles
}

void
ProcessMaster::handleSignalWait()
{
    // No-op on windows, uses waitable object handles
}

class ProcessExitEvent::Impl
{
    std::shared_ptr<RealTimer> mOuterTimer;
    std::shared_ptr<asio::error_code> mOuterEc;
    asio::windows::object_handle mObjHandle;

  public:
    Impl(std::shared_ptr<RealTimer> const& outerTimer,
         std::shared_ptr<asio::error_code> const& outerEc, HANDLE hProcess,
         HANDLE hThread)
        : mOuterTimer(outerTimer)
        , mOuterEc(outerEc)
        , mObjHandle(outerTimer->get_io_service(), hProcess)
    {
        auto ot = mOuterTimer;
        auto oe = mOuterEc;
        mObjHandle.async_wait([hProcess, hThread, ot, oe](asio::error_code ec)
                              {
                                  CloseHandle(hProcess);
                                  CloseHandle(hThread);
                                  *oe = ec;
                                  ot->cancel();
                              });
    }
};

ProcessExitEvent
ProcessMaster::runProcess(std::string const& cmdLine)
{
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);
    LPSTR cmd = (LPSTR)cmdLine.data();

    LOG(DEBUG) << "Starting process: " << cmdLine;
    if (!CreateProcess(NULL,    // No module name (use command line)
                       cmd,     // Command line
                       nullptr, // Process handle not inheritable
                       nullptr, // Thread handle not inheritable
                       FALSE,   // Set handle inheritance to FALSE
                       0,       // No creation flags
                       nullptr, // Use parent's environment block
                       nullptr, // Use parent's starting directory
                       &si,     // Pointer to STARTUPINFO structure
                       &pi)     // Pointer to PROCESS_INFORMATION structure
        )
    {
        LOG(DEBUG) << "CreateProcess() failed: " << GetLastError();
        throw std::runtime_error("CreateProcess() failed");
    }

    auto& svc = mApp.getMainIOService();
    ProcessExitEvent pe(svc);
    pe.mImpl = std::make_shared<ProcessExitEvent::Impl>(
        pe.mTimer, pe.mEc, pi.hProcess, pi.hThread);
    return pe;
}

#else

#include <spawn.h>
#include <sys/wait.h>

class ProcessExitEvent::Impl
{
  public:
    std::shared_ptr<RealTimer> mOuterTimer;
    std::shared_ptr<asio::error_code> mOuterEc;
    Impl(std::shared_ptr<RealTimer> const& outerTimer,
         std::shared_ptr<asio::error_code> const& outerEc)
        : mOuterTimer(outerTimer), mOuterEc(outerEc)
    {
    }
};

ProcessMaster::ProcessMaster(Application& app)
    : mApp(app), mSigChild(app.getMainIOService(), SIGCHLD)
{
    startSignalWait();
}

void
ProcessMaster::startSignalWait()
{
    mSigChild.async_wait(std::bind(&ProcessMaster::handleSignalWait, this));
}

void
ProcessMaster::handleSignalWait()
{
    while (true)
    {
        int status = 0;
        int pid = waitpid(-1, &status, WNOHANG);
        if (pid > 0)
        {
            asio::error_code ec;
            if (WIFEXITED(status))
            {
                // FIXME: this doesn't _quite_ do the right thing; it conveys
                // the exit status back to the caller but it puts it in "system
                // category" which on POSIX means if you call .message() on it
                // you'll get perror(value()), which is not correct. Errno has
                // nothing to do with process exit values. We could make a new
                // error_category to tighten this up, but it's a bunch of work
                // just to convey the meaningless string "exited" to the user.
                ec = asio::error_code(WEXITSTATUS(status),
                                      asio::system_category());
            }
            else
            {
                // FIXME: for now we also collapse all non-WIFEXITED exits on
                // posix into a single "exit 1" error_code. This is enough
                // for most callers; we can enrich it if anyone really wants
                // to differentiate various signals that might have killed
                // the child.
                ec = asio::error_code(1, asio::system_category());
            }
            auto pair = mImpls.find(pid);
            if (pair != mImpls.end())
            {
                auto impl = pair->second;
                mImpls.erase(pair);
                *(impl->mOuterEc) = ec;
                impl->mOuterTimer->cancel();
            }
        }
        else
        {
            break;
        }
    }
    startSignalWait();
}

static std::vector<std::string>
split(const std::string& s)
{
    std::vector<std::string> parts;
    auto delim = " ";
    auto a = s.find_first_not_of(delim, 0);
    auto b = s.find_first_of(delim, a);
    while (b != std::string::npos || a != std::string::npos)
    {
        auto len = b == std::string::npos ? std::string::npos : b - a;
        parts.push_back(s.substr(a, len));
        a = s.find_first_not_of(delim, b);
        b = s.find_first_of(delim, a);
    }
    return parts;
}

ProcessExitEvent
ProcessMaster::runProcess(std::string const& cmdLine)
{
    std::vector<std::string> args = split(cmdLine);
    std::vector<char*> argv;
    for (auto& a : args)
    {
        argv.push_back((char*)a.data());
    }
    argv.push_back(nullptr);
    char* env[1] = {nullptr};
    int pid;

    LOG(DEBUG) << "Starting process: " << cmdLine;
    int err = posix_spawnp(&pid, argv[0],
                           nullptr, // posix_spawn_file_actions_t
                           nullptr, // posix_spawnattr_t*
                           argv.data(), env);
    if (err)
    {
        LOG(DEBUG) << "posix_spawn() failed: " << strerror(err);
        throw std::runtime_error("posix_spawn() failed");
    }

    auto& svc = mApp.getMainIOService();
    ProcessExitEvent pe(svc);
    pe.mImpl = std::make_shared<ProcessExitEvent::Impl>(pe.mTimer, pe.mEc);
    mImpls[pid] = pe.mImpl;
    return pe;
}

#endif

ProcessExitEvent::ProcessExitEvent(asio::io_service& io_service)
    : mTimer(std::make_shared<RealTimer>(io_service))
    , mImpl(nullptr)
    , mEc(std::make_shared<asio::error_code>())
{
    mTimer->expires_from_now(std::chrono::steady_clock::duration::max());
}

ProcessExitEvent::~ProcessExitEvent()
{
}

void
ProcessExitEvent::async_wait(std::function<void(asio::error_code)>
                             const& handler)
{
    {
        // Unfortunately when you cancel a timer, asio delivers
        // asio::error::operation_aborted to all the waiters, even if you pass a
        // different error_code to the cancel() call. So we have to route the
        // _actual_ process-exit condition through _another_ variable shared
        // between ProcessExitEvent and the per-platform handlers.
        auto ec = mEc;
        std::function<void(asio::error_code)> h(handler);
        mTimer->async_wait([ec, h](asio::error_code)
                           {
                               h(*ec);
                           });
    }
}


}
