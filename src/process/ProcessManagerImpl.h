#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "process/ProcessManager.h"
#include <deque>
#include <mutex>

namespace medida
{
class Counter;
}

namespace stellar
{

class ProcessManagerImpl : public ProcessManager
{
    // Subprocess callbacks are process-wide, owing to the process-wide
    // receipt of SIGCHLD, at least on POSIX.
    static std::recursive_mutex gImplsMutex;
    static std::map<int, std::shared_ptr<ProcessExitEvent::Impl>> gImpls;

    // On windows we use a simple global counter to throttle the
    // number of processes we run at once.
    static size_t gNumProcessesActive;

    bool mIsShutdown{false};
    size_t mMaxProcesses;
    asio::io_service& mIOService;

    std::deque<std::shared_ptr<ProcessExitEvent::Impl>> mPendingImpls;
    void maybeRunPendingProcesses();

    // These are only used on POSIX, but they're harmless here.
    asio::signal_set mSigChild;
    void startSignalWait();
    void handleSignalWait();

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
