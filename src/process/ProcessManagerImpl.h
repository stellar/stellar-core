#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "process/ProcessManager.h"
#include <atomic>
#include <deque>
#include <mutex>
#include <vector>

namespace medida
{
class Counter;
}

namespace stellar
{

class ProcessManagerImpl : public ProcessManager
{
    // Subprocess callbacks are process-wide, owing to the process-wide
    // receipt of SIGCHLD, at least on POSIX. The list of all active
    // process mangers is kept globally so as to dispatch the child
    // process stop signal accordingly
    static std::recursive_mutex gManagersMutex;
    static std::vector<ProcessManagerImpl*> gManagers;

    // On windows we use a simple global counter to throttle the
    // number of processes we run at once.
    static std::atomic<size_t> gNumProcessesActive;

    // Subprocesses will be removed asynchronously, hence the lock on
    // just this member
    std::recursive_mutex mImplsMutex;
    std::map<int, std::shared_ptr<ProcessExitEvent::Impl>> mImpls;

    bool mIsShutdown{false};
    size_t mMaxProcesses;
    asio::io_service& mIOService;

    std::deque<std::shared_ptr<ProcessExitEvent::Impl>> mPendingImpls;
    void maybeRunPendingProcesses();
    void registerManager();

    // These are only used on POSIX, but they're harmless here.
    asio::signal_set mSigChild;
    void startSignalWait();
    void handleSignalWait();
    bool handleProcessTermination(int pid, int status);

    friend class ProcessExitEvent::Impl;

  public:
    ProcessManagerImpl(Application& app);
    ProcessExitEvent runProcess(std::string const& cmdLine,
                                std::string outFile = "") override;
    size_t getNumRunningProcesses() override;

    bool isShutdown() const override;
    void shutdown() override;

    ~ProcessManagerImpl() override;
};
}
