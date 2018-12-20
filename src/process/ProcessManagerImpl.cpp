// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#define STELLAR_CORE_REAL_TIMER_FOR_CERTAIN_NOT_JUST_VIRTUAL_TIME
#include "process/ProcessManagerImpl.h"
// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"

#include "main/Application.h"
#include "main/Config.h"
#include "process/PosixSpawnFileActions.h"
#include "process/ProcessManager.h"
#include "process/ProcessManagerImpl.h"
#include "util/Logging.h"
#include "util/Timer.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <mutex>
#include <regex>
#include <string>

#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#endif

#ifdef __APPLE__
extern char** environ;
#endif

namespace stellar
{

static const asio::error_code ABORT_ERROR_CODE(asio::error::operation_aborted,
                                               asio::system_category());

std::shared_ptr<ProcessManager>
ProcessManager::create(Application& app)
{
    return std::make_shared<ProcessManagerImpl>(app);
}

std::atomic<size_t> ProcessManagerImpl::gNumProcessesActive{0};

class ProcessExitEvent::Impl
    : public std::enable_shared_from_this<ProcessExitEvent::Impl>
{
  public:
    std::shared_ptr<RealTimer> mOuterTimer;
    std::shared_ptr<asio::error_code> mOuterEc;
    std::string mCmdLine;
    std::string mOutFile;
    bool mRunning{false};
#ifdef _WIN32
    asio::windows::object_handle mProcessHandle;
#endif
    std::weak_ptr<ProcessManagerImpl> mProcManagerImpl;
    int mProcessId{-1};

    Impl(std::shared_ptr<RealTimer> const& outerTimer,
         std::shared_ptr<asio::error_code> const& outerEc,
         std::string const& cmdLine, std::string const& outFile,
         std::weak_ptr<ProcessManagerImpl> pm)
        : mOuterTimer(outerTimer)
        , mOuterEc(outerEc)
        , mCmdLine(cmdLine)
        , mOutFile(outFile)
#ifdef _WIN32
        , mProcessHandle(outerTimer->get_io_service())
#endif
        , mProcManagerImpl(pm)
    {
    }
    void run();
    void
    cancel(asio::error_code const& ec)
    {
        *mOuterEc = ec;
        mOuterTimer->cancel();
    }

    int
    getProcessId() const
    {
        return mProcessId;
    }
};

size_t
ProcessManagerImpl::getNumRunningProcesses()
{
    return gNumProcessesActive;
}

ProcessManagerImpl::~ProcessManagerImpl()
{
    const auto killProcess = [&](ProcessExitEvent::Impl& impl) {
        impl.cancel(ABORT_ERROR_CODE);
        forceShutdown(impl);
    };
    // Use SIGKILL on any processes we already used SIGINT on
    while (!mKillableImpls.empty())
    {
        auto impl = std::move(mKillableImpls.front());
        mKillableImpls.pop_front();
        killProcess(*impl);
    }
    // Use SIGKILL on any processes we haven't politely asked to exit yet
    for (auto& pair : mImpls)
    {
        killProcess(*pair.second);
    }
}

bool
ProcessManagerImpl::isShutdown() const
{
    return mIsShutdown;
}

void
ProcessManagerImpl::shutdown()
{
    if (!mIsShutdown)
    {
        mIsShutdown = true;
        auto ec = ABORT_ERROR_CODE;

        // Cancel all pending.
        std::lock_guard<std::recursive_mutex> guard(mImplsMutex);
        for (auto& pending : mPendingImpls)
        {
            pending->cancel(ec);
        }
        mPendingImpls.clear();

        // Cancel all running.
        for (auto& pair : mImpls)
        {
            // Mark it as "ready to be killed"
            mKillableImpls.push_back(pair.second);
            // Cancel any pending events and shut down the process cleanly
            pair.second->cancel(ec);
            cleanShutdown(*pair.second);
        }
        mImpls.clear();
        gNumProcessesActive = 0;
#ifndef _WIN32
        mSigChild.cancel(ec);
#endif
    }
}

#ifdef _WIN32
#include <tchar.h>
#include <windows.h>

ProcessManagerImpl::ProcessManagerImpl(Application& app)
    : mMaxProcesses(app.getConfig().MAX_CONCURRENT_SUBPROCESSES)
    , mIOService(app.getClock().getIOService())
    , mSigChild(mIOService)
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
    auto manager = mProcManagerImpl.lock();
    assert(manager && !manager->isShutdown());
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
                       CREATE_NEW_PROCESS_GROUP, // Create a new process group
                       nullptr, // Use parent's environment block
                       nullptr, // Use parent's starting directory
                       &si,     // Pointer to STARTUPINFO structure
                       &pi)     // Pointer to PROCESS_INFORMATION structure
    )
    {
        if (si.hStdOutput != NULL)
        {
            CloseHandle(si.hStdOutput);
        }
        CLOG(ERROR, "Process") << "CreateProcess() failed: " << GetLastError();
        throw std::runtime_error("CreateProcess() failed");
    }
    CloseHandle(si.hStdOutput);
    CloseHandle(pi.hThread); // we don't need this handle
    pi.hThread = INVALID_HANDLE_VALUE;

    mProcessHandle.assign(pi.hProcess);
    mProcessId = pi.dwProcessId;

    // capture a shared pointer to "this" to keep Impl alive until the end
    // of the execution
    auto sf = shared_from_this();
    mProcessHandle.async_wait([sf](asio::error_code ec) {
        auto manager = sf->mProcManagerImpl.lock();
        if (!manager || manager->isShutdown())
        {
            return;
        }

        --ProcessManagerImpl::gNumProcessesActive;

        // Fire off any new processes we've made room for before we
        // trigger the callback.
        manager->maybeRunPendingProcesses();

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
            ec = asio::error_code(exitCode, asio::system_category());
        }
        manager->handleProcessTermination(sf->mProcessId, ec.value());
        sf->cancel(ec);
    });
    mRunning = true;
}

void
ProcessManagerImpl::handleProcessTermination(int pid, int /*status*/)
{
    std::lock_guard<std::recursive_mutex> guard(mImplsMutex);
    mImpls.erase(pid);
}

void
ProcessManagerImpl::cleanShutdown(ProcessExitEvent::Impl& impl)
{
    if (!GenerateConsoleCtrlEvent(CTRL_C_EVENT, impl.getProcessId()))
    {
        CLOG(WARNING, "Process")
            << "failed to cleanly shutdown process with pid "
            << impl.getProcessId() << ", error code " << GetLastError();
    }
}

void
ProcessManagerImpl::forceShutdown(ProcessExitEvent::Impl& impl)
{
    if (!TerminateProcess(impl.mProcessHandle.native_handle(), 1))
    {
        CLOG(WARNING, "Process")
            << "failed to force shutdown of process with pid "
            << impl.getProcessId() << ", error code " << GetLastError();
    }
    // Cancel any pending events on the handle. Ignore error code
    asio::error_code dummy;
    impl.mProcessHandle.cancel(dummy);
}

#else

#include <spawn.h>
#include <sys/wait.h>

ProcessManagerImpl::ProcessManagerImpl(Application& app)
    : mMaxProcesses(app.getConfig().MAX_CONCURRENT_SUBPROCESSES)
    , mIOService(app.getClock().getIOService())
    , mSigChild(mIOService, SIGCHLD)
{
    std::lock_guard<std::recursive_mutex> guard(mImplsMutex);
    startSignalWait();
}

void
ProcessManagerImpl::startSignalWait()
{
    std::lock_guard<std::recursive_mutex> guard(mImplsMutex);
    mSigChild.async_wait(
        std::bind(&ProcessManagerImpl::handleSignalWait, this));
}

void
ProcessManagerImpl::handleSignalWait()
{
    if (isShutdown())
    {
        return;
    }
    // Store tuples (pid, status)
    std::vector<std::tuple<int, int>> signaledChildren;
    std::lock_guard<std::recursive_mutex> guard(mImplsMutex);
    for (auto const& implPair : mImpls)
    {
        const int pid = implPair.first;
        int status = 0;
        // If we find the child for which we received this SIGCHLD signal,
        // store the pid and status
        if (waitpid(pid, &status, WNOHANG) > 0)
        {
            signaledChildren.push_back(std::make_tuple(pid, status));
        }
    }

    if (!signaledChildren.empty())
    {
        CLOG(DEBUG, "Process") << "found " << signaledChildren.size()
                               << " child processes that terminated";
        // Now go all over all (pid, status) and handle them
        for (auto const& pidStatus : signaledChildren)
        {
            const int pid = std::get<0>(pidStatus);
            const int status = std::get<1>(pidStatus);
            handleProcessTermination(pid, status);
        }
    }
    startSignalWait();
}

void
ProcessManagerImpl::handleProcessTermination(int pid, int status)
{
    std::lock_guard<std::recursive_mutex> guard(mImplsMutex);
    auto pair = mImpls.find(pid);
    if (pair == mImpls.end())
    {
        CLOG(DEBUG, "Process") << "failed to find process with pid " << pid;
        return;
    }
    auto impl = pair->second;

    asio::error_code ec;
    if (WIFEXITED(status))
    {
        if (WEXITSTATUS(status) == 0)
        {
            CLOG(DEBUG, "Process")
                << "process " << pid << " exited " << WEXITSTATUS(status)
                << ": " << impl->mCmdLine;
        }
        else
        {
            CLOG(WARNING, "Process")
                << "process " << pid << " exited " << WEXITSTATUS(status)
                << ": " << impl->mCmdLine;
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
        ec = asio::error_code(WEXITSTATUS(status), asio::system_category());
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
    mImpls.erase(pair);

    // Fire off any new processes we've made room for before we
    // trigger the callback.
    maybeRunPendingProcesses();

    impl->cancel(ec);
}

void
ProcessManagerImpl::cleanShutdown(ProcessExitEvent::Impl& impl)
{
    const int pid = impl.getProcessId();
    if (kill(pid, SIGINT) != 0)
    {
        CLOG(WARNING, "Process")
            << "kill (SIGINT) failed for pid " << pid << ", errno " << errno;
    }
}

void
ProcessManagerImpl::forceShutdown(ProcessExitEvent::Impl& impl)
{
    const int pid = impl.getProcessId();
    if (kill(pid, SIGKILL) != 0)
    {
        CLOG(WARNING, "Process")
            << "kill (SIGKILL) failed for pid " << pid << ", errno " << errno;
    }
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
    auto manager = mProcManagerImpl.lock();
    assert(manager && !manager->isShutdown());
    if (mRunning)
    {
        CLOG(ERROR, "Process") << "ProcessExitEvent::Impl already running";
        throw std::runtime_error("ProcessExitEvent::Impl already running");
    }
    std::vector<std::string> args = split(mCmdLine);
    std::vector<char*> argv;
    for (auto& a : args)
    {
        argv.push_back((char*)a.data());
    }
    argv.push_back(nullptr);
    int err = 0;

    PosixSpawnFileActions fileActions;
    if (!mOutFile.empty())
    {
        fileActions.addOpen(1, mOutFile, O_RDWR | O_CREAT, 0600);
    }
    // Iterate through all possibly open file descriptors except stdin, stdout,
    // and stderr and set FD_CLOEXEC so the subprocess doesn't inherit them
    const int maxFds = sysconf(_SC_OPEN_MAX);
    // as the space of open file descriptors is arbitrary large
    // we use as a heuristic the number of consecutive unused descriptors
    // as an indication that we're past the range where descriptors are
    // allocated
    // a better way would be to enumerate the opened descriptors, but there
    // doesn't seem to be a portable way to do this
    const int maxGAP = 512;
    for (int fd = 3, lastFd = 3; (fd < maxFds) && ((fd - lastFd) < maxGAP);
         ++fd)
    {
        int flags = fcntl(fd, F_GETFD);
        if (flags != -1)
        {
            // set if it was not already set
            if ((flags & FD_CLOEXEC) == 0)
            {
                fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
            }
            lastFd = fd;
        }
    }
    err = posix_spawnp(&mProcessId, argv[0], fileActions,
                       nullptr, // posix_spawnattr_t*
                       argv.data(), environ);
    if (err)
    {
        CLOG(ERROR, "Process") << "posix_spawn() failed: " << strerror(err);
        throw std::runtime_error("posix_spawn() failed");
    }

    mRunning = true;
}

#endif

ProcessExitEvent
ProcessManagerImpl::runProcess(std::string const& cmdLine, std::string outFile)
{
    std::lock_guard<std::recursive_mutex> guard(mImplsMutex);
    ProcessExitEvent pe(mIOService);
    std::shared_ptr<ProcessManagerImpl> self =
        std::static_pointer_cast<ProcessManagerImpl>(shared_from_this());
    std::weak_ptr<ProcessManagerImpl> weakSelf(self);
    pe.mImpl = std::make_shared<ProcessExitEvent::Impl>(
        pe.mTimer, pe.mEc, cmdLine, outFile, weakSelf);
    mPendingImpls.push_back(pe.mImpl);

    maybeRunPendingProcesses();
    return pe;
}

void
ProcessManagerImpl::maybeRunPendingProcesses()
{
    if (mIsShutdown)
    {
        return;
    }
    std::lock_guard<std::recursive_mutex> guard(mImplsMutex);
    while (!mPendingImpls.empty() && gNumProcessesActive < mMaxProcesses)
    {
        auto i = mPendingImpls.front();
        mPendingImpls.pop_front();
        try
        {
            CLOG(DEBUG, "Process") << "Running: " << i->mCmdLine;

            i->run();
            mImpls[i->getProcessId()] = i;
            ++gNumProcessesActive;
        }
        catch (std::runtime_error& e)
        {
            i->cancel(std::make_error_code(std::errc::io_error));
            CLOG(ERROR, "Process") << "Error starting process: " << e.what();
            CLOG(ERROR, "Process") << "When running: " << i->mCmdLine;
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
    mTimer->async_wait([ec, h](asio::error_code) { h(*ec); });
}
}
