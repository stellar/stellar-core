// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#define STELLAR_CORE_REAL_TIMER_FOR_CERTAIN_NOT_JUST_VIRTUAL_TIME
#include "process/ProcessManagerImpl.h"
// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"

#include "util/Timer.h"
#include "main/Application.h"
#include "main/Config.h"
#include "util/Logging.h"
#include "util/make_unique.h"
#include "process/ProcessManager.h"
#include "process/ProcessManagerImpl.h"

#include "medida/counter.h"
#include "medida/metrics_registry.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <mutex>
#include <regex>
#include <string>

namespace stellar
{

std::unique_ptr<ProcessManager>
ProcessManager::create(Application& app)
{
    return make_unique<ProcessManagerImpl>(app);
}

std::recursive_mutex ProcessManagerImpl::gImplsMutex;

std::map<int, std::shared_ptr<ProcessExitEvent::Impl>>
    ProcessManagerImpl::gImpls;

size_t ProcessManagerImpl::gNumProcessesActive = 0;

size_t
ProcessManagerImpl::getNumRunningProcesses()
{
    std::lock_guard<std::recursive_mutex> guard(gImplsMutex);
    return gNumProcessesActive;
}

ProcessManagerImpl::~ProcessManagerImpl()
{
}

class ProcessExitEvent::Impl
    : public std::enable_shared_from_this<ProcessExitEvent::Impl>
{
  public:
    std::shared_ptr<RealTimer> mOuterTimer;
    std::shared_ptr<asio::error_code> mOuterEc;
    std::string mCmdLine;
    std::string mOutFile;
    bool mRunning{false};
#ifdef _MSC_VER
    asio::windows::object_handle mProcessHandle;
#endif

    Impl(std::shared_ptr<RealTimer> const& outerTimer,
         std::shared_ptr<asio::error_code> const& outerEc,
         std::string const& cmdLine,
         std::string const& outFile)
        : mOuterTimer(outerTimer)
        , mOuterEc(outerEc)
        , mCmdLine(cmdLine)
        , mOutFile(outFile)
#ifdef _MSC_VER
        , mProcessHandle(outerTimer->get_io_service())
#endif
        {
        }
    void run();
};

#ifdef _MSC_VER
#include <windows.h>
#include <tchar.h>

ProcessManagerImpl::ProcessManagerImpl(Application& app)
    : mApp(app)
    , mSigChild(app.getClock().getIOService())
    , mImplsSize(app.getMetrics().NewCounter({"process", "memory", "handles"}))
{
}

void
ProcessManagerImpl::startSignalWait()
{
    // No-op on windows, uses waitable object handles
}

void
ProcessManagerImpl::handleSignalWait()
{
    // No-op on windows, uses waitable object handles
}

void
ProcessExitEvent::Impl::run()
{
    if (mRunning)
    {
        CLOG(ERROR, "Process") << "ProcessExitEvent::Impl already running";
        throw std::runtime_error("ProcessExitEvent::Impl already running");
    }

    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);
    LPSTR cmd = (LPSTR)mCmdLine.data();

    if (!mOutFile.empty())
    {
        SECURITY_ATTRIBUTES sa;
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = NULL;
        sa.bInheritHandle = TRUE;

        si.cb = sizeof(STARTUPINFO);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput =
            CreateFile((LPCTSTR)mOutFile.c_str(),          // name of the file
                       GENERIC_WRITE,                      // open for writing
                       FILE_SHARE_WRITE | FILE_SHARE_READ, // share r/w access
                       &sa,                   // security attributes
                       CREATE_ALWAYS,         // overwrite if existing
                       FILE_ATTRIBUTE_NORMAL, // normal file
                       NULL);                 // no attr. template
        if (si.hStdOutput == INVALID_HANDLE_VALUE)
        {
            CLOG(ERROR, "Process") << "CreateFile() failed: " << GetLastError();
            throw std::runtime_error("CreateFile() failed");
        }
    }

    if (!CreateProcess(NULL,    // No module name (use command line)
                       cmd,     // Command line
                       nullptr, // Process handle not inheritable
                       nullptr, // Thread handle not inheritable
                       TRUE,    // Inherit file handles
                       0,       // No creation flags
                       nullptr, // Use parent's environment block
                       nullptr, // Use parent's starting directory
                       &si,     // Pointer to STARTUPINFO structure
                       &pi)     // Pointer to PROCESS_INFORMATION structure
        )
    {
        CLOG(ERROR, "Process") << "CreateProcess() failed: " << GetLastError();
        throw std::runtime_error("CreateProcess() failed");
    }
    CloseHandle(si.hStdOutput);
    CloseHandle(pi.hThread); // we don't need this handle
    pi.hThread = INVALID_HANDLE_VALUE;

    mProcessHandle.assign(pi.hProcess);

    // capture a shared pointer to "this" to keep Impl alive until the end
    // of the execution
    auto sf = shared_from_this();
    mProcessHandle.async_wait(
        [sf](asio::error_code ec)
        {
            {
                std::lock_guard<std::recursive_mutex>
                    guard(ProcessManagerImpl::gImplsMutex);
                --ProcessManagerImpl::gNumProcessesActive;
            }
            if (ec)
            {
                *(sf->mOuterEc) = ec;
            }
            else
            {
                DWORD exitCode;
                BOOL res = GetExitCodeProcess(sf->mProcessHandle.native_handle(),
                                              &exitCode);
                if (!res)
                {
                    exitCode = 1;
                }
                *(sf->mOuterEc) =
                    asio::error_code(exitCode, asio::system_category());
            }
            sf->mOuterTimer->cancel();
        });
    mRunning = true;
}

#else

#include <spawn.h>
#include <sys/wait.h>

ProcessManagerImpl::ProcessManagerImpl(Application& app)
    : mApp(app)
    , mSigChild(app.getClock().getIOService(), SIGCHLD)
    , mImplsSize(app.getMetrics().NewCounter({"process", "memory", "handles"}))
{
    std::lock_guard<std::recursive_mutex> guard(gImplsMutex);
    startSignalWait();
}

void
ProcessManagerImpl::startSignalWait()
{
    std::lock_guard<std::recursive_mutex> guard(gImplsMutex);
    mSigChild.async_wait(
        std::bind(&ProcessManagerImpl::handleSignalWait, this));
}

void
ProcessManagerImpl::handleSignalWait()
{
    std::lock_guard<std::recursive_mutex> guard(gImplsMutex);
    for (;;)
    {
        int status = 0;
        int pid = waitpid(-1, &status, WNOHANG);
        if (pid > 0)
        {
            auto pair = gImpls.find(pid);
            assert(pair != gImpls.end());
            auto impl = pair->second;

            asio::error_code ec;
            if (WIFEXITED(status))
            {
                if (WEXITSTATUS(status) == 0)
                {
                    CLOG(DEBUG, "Process") << "process " << pid << " exited "
                                           << WEXITSTATUS(status) << ": "
                                           << impl->mCmdLine;
                }
                else
                {
                    CLOG(WARNING, "Process") << "process " << pid << " exited "
                                             << WEXITSTATUS(status) << ": "
                                             << impl->mCmdLine;
                }
#ifdef __linux__
                // Linux posix_spawnp does not fault on file-not-found in the
                // parent process at the point of invocation, as BSD does; so
                // rather than a fatal error / throw we get an ambiguous and
                // easily-overlooked shell-like 'exit 127' on waitpid.
                if (WEXITSTATUS(status) == 127)
                {
                    CLOG(WARNING, "Process") << "";
                    CLOG(WARNING, "Process") << "************";
                    CLOG(WARNING, "Process") << "";
                    CLOG(WARNING, "Process") << "  likely 'missing command':";
                    CLOG(WARNING, "Process") << "";
                    CLOG(WARNING, "Process") << "    " << impl->mCmdLine;
                    CLOG(WARNING, "Process") << "";
                    CLOG(WARNING, "Process") << "************";
                    CLOG(WARNING, "Process") << "";
                }
#endif
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

            --gNumProcessesActive;
            gImpls.erase(pair);

            // Fire off any new processes we've made room for before we
            // trigger the callback.
            mImplsSize.set_count(gImpls.size());
            maybeRunPendingProcesses();

            *(impl->mOuterEc) = ec;
            impl->mOuterTimer->cancel();
        }
        else
        {
            break;
        }
    }
    startSignalWait();
}

static std::vector<std::string>
split(std::string const& s)
{
    std::vector<std::string> parts;
    std::regex ws_re("\\s+");
    std::copy(std::sregex_token_iterator(s.begin(), s.end(), ws_re, -1),
              std::sregex_token_iterator(), std::back_inserter(parts));
    return parts;
}

void
ProcessExitEvent::Impl::run()
{
    if (mRunning)
    {
        CLOG(ERROR, "Process") << "ProcessExitEvent::Impl already running";
        throw std::runtime_error("ProcessExitEvent::Impl already running");
    }
    std::lock_guard<std::recursive_mutex> guard(ProcessManagerImpl::gImplsMutex);
    std::vector<std::string> args = split(mCmdLine);
    std::vector<char*> argv;
    for (auto& a : args)
    {
        argv.push_back((char*)a.data());
    }
    argv.push_back(nullptr);
    char* env[1] = {nullptr};
    int pid, err = 0;

    posix_spawn_file_actions_t fileActions;
    if (!mOutFile.empty())
    {
        err = posix_spawn_file_actions_init(&fileActions);
        if (err)
        {
            CLOG(ERROR, "Process")
                << "posix_spawn_file_actions_init() failed: " << strerror(err);
            throw std::runtime_error("posix_spawn_file_actions_init() failed");
        }
        err = posix_spawn_file_actions_addopen(&fileActions, 1, mOutFile.c_str(),
                                               O_RDWR | O_CREAT, 0600);
        if (err)
        {
            CLOG(ERROR, "Process")
                << "posix_spawn_file_actions_addopen() failed: "
                << strerror(err);
            throw std::runtime_error(
                "posix_spawn_file_actions_addopen() failed");
        }
    }

    err = posix_spawnp(&pid, argv[0], mOutFile.empty() ? nullptr : &fileActions,
                       nullptr, // posix_spawnattr_t*
                       argv.data(), env);
    if (err)
    {
        CLOG(ERROR, "Process") << "posix_spawn() failed: " << strerror(err);
        throw std::runtime_error("posix_spawn() failed");
    }

    if (!mOutFile.empty())
    {
        err = posix_spawn_file_actions_destroy(&fileActions);
        if (err)
        {
            CLOG(ERROR, "Process")
                << "posix_spawn_file_actions_destroy() failed: "
                << strerror(err);
            throw std::runtime_error(
                "posix_spawn_file_actions_destroy() failed");
        }
    }
    ProcessManagerImpl::gImpls[pid] = shared_from_this();
    mRunning = true;
}

#endif

ProcessExitEvent
ProcessManagerImpl::runProcess(std::string const& cmdLine, std::string outFile)
{
    std::lock_guard<std::recursive_mutex> guard(gImplsMutex);
    auto& svc = mApp.getClock().getIOService();
    ProcessExitEvent pe(svc);
    pe.mImpl = std::make_shared<ProcessExitEvent::Impl>(pe.mTimer, pe.mEc,
                                                        cmdLine, outFile);
    mPendingImpls.push_back(pe.mImpl);
    maybeRunPendingProcesses();
    mImplsSize.set_count(gNumProcessesActive);
    return pe;
}

void
ProcessManagerImpl::maybeRunPendingProcesses()
{
    std::lock_guard<std::recursive_mutex> guard(gImplsMutex);
    while (!mPendingImpls.empty() &&
           gNumProcessesActive < mApp.getConfig().MAX_CONCURRENT_SUBPROCESSES)
    {
        auto i = mPendingImpls.front();
        mPendingImpls.pop_front();
        try
        {
            CLOG(DEBUG, "Process") << "Running: " << i->mCmdLine;
            i->run();
            ++gNumProcessesActive;
        }
        catch (std::runtime_error& e)
        {
            CLOG(ERROR, "Process")
                << "Error staring process: " << e.what();
            CLOG(ERROR, "Process")
                << "When running: " << i->mCmdLine;
        }
    }
}

ProcessExitEvent::ProcessExitEvent(asio::io_service& io_service)
    : mTimer(std::make_shared<RealTimer>(io_service))
    , mImpl(nullptr)
    , mEc(std::make_shared<asio::error_code>())
{
    mTimer->expires_from_now(std::chrono::system_clock::duration::max());
}

ProcessExitEvent::~ProcessExitEvent()
{
}

void
ProcessExitEvent::async_wait(
    std::function<void(asio::error_code)> const& handler)
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
