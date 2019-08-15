#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "process/ProcessManager.h"
#include "util/TmpDir.h"
#include <atomic>
#include <deque>
#include <mutex>
#include <vector>

namespace stellar
{

class ProcessManagerImpl : public ProcessManager
{
    // On windows we use a simple global counter to throttle the
    // number of processes we run at once.
    static std::atomic<size_t> gNumProcessesActive;

    // Subprocesses will be removed asynchronously, hence the lock on
    // just this member
    std::recursive_mutex mProcessesMutex;
    std::map<int, std::shared_ptr<ProcessExitEvent>> mProcesses;

    bool mIsShutdown{false};
    size_t mMaxProcesses;
    asio::io_context& mIOContext;
    // These are only used on POSIX, but they're harmless here.
    asio::signal_set mSigChild;
    std::unique_ptr<TmpDir> mTmpDir;
    uint64_t mTempFileCount{0};

    std::deque<std::shared_ptr<ProcessExitEvent>> mPending;
    std::deque<std::shared_ptr<ProcessExitEvent>> mKillable;
    void maybeRunPendingProcesses();

    void startSignalWait();
    void handleSignalWait();
    asio::error_code handleProcessTermination(int pid, int status);
    bool cleanShutdown(ProcessExitEvent& pe);
    bool forceShutdown(ProcessExitEvent& pe);

    friend class ProcessExitEvent::Impl;

  public:
    explicit ProcessManagerImpl(Application& app);
    std::weak_ptr<ProcessExitEvent> runProcess(std::string const& cmdLine,
                                               std::string outFile) override;
    size_t getNumRunningProcesses() override;

    bool isShutdown() const override;
    void shutdown() override;
    bool tryProcessShutdown(std::shared_ptr<ProcessExitEvent> pe) override;

    ~ProcessManagerImpl() override;
};
}
