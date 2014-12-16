#include "main/Application.h"
#include "lib/util/Logging.h"

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
    std::shared_ptr<Timer> mOuterTimer;
    std::shared_ptr<asio::error_code> mOuterEc;
    asio::windows::object_handle mObjHandle;

  public:
    Impl(std::shared_ptr<Timer> const& outerTimer,
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
    std::shared_ptr<Timer> mOuterTimer;
    std::shared_ptr<asio::error_code> mOuterEc;
    Impl(std::shared_ptr<Timer> const& outerTimer,
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
    mSigChild.async_wait(bind(&ProcessMaster::handleSignalWait, this));
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
    while (b != string::npos || a != string::npos)
    {
        auto len = b == string::npos ? string::npos : b - a;
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
    : mTimer(std::make_shared<Timer>(io_service))
    , mImpl(nullptr)
    , mEc(std::make_shared<asio::error_code>())
{
    mTimer->expires_from_now(std::chrono::steady_clock::duration::max());
}

ProcessExitEvent::~ProcessExitEvent()
{
}
}
