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
    // Subprocesses will be removed asynchronously, hence the lock on
    // just the mProcesses member.
    std::recursive_mutex mProcessesMutex;

    // Stores a map from pid to running-or-shutting-down processes.
    // Any ProcessExitEvent should be stored either in mProcesses
    // or in mPending (before it's launched).
    std::map<int, std::shared_ptr<ProcessExitEvent>> mProcesses;

    bool mIsShutdown{false};
    size_t const mMaxProcesses;
    asio::io_context& mIOContext;
    // These are only used on POSIX, but they're harmless here.
    asio::signal_set mSigChild;
    std::unique_ptr<TmpDir> mTmpDir;
    uint64_t mTempFileCount{0};

    std::deque<std::shared_ptr<ProcessExitEvent>> mPending;
    void maybeRunPendingProcesses();
    void checkInvariants();

    void startWaitingForSignalChild();
    void handleSignalChild();
    void reapChildren();
    asio::error_code handleProcessTermination(int pid, int status);
    bool politeShutdown(std::shared_ptr<ProcessExitEvent> pe);
    bool forcedShutdown(std::shared_ptr<ProcessExitEvent> pe);
    void tryProcessShutdownAll();

    friend class ProcessExitEvent::Impl;

  public:
    explicit ProcessManagerImpl(Application& app);
    std::weak_ptr<ProcessExitEvent> runProcess(std::string const& cmdLine,
                                               std::string outFile) override;
    size_t getNumRunningProcesses() override;
    size_t getNumRunningOrShuttingDownProcesses() override;

    bool isShutdown() const override;
    void shutdown() override;
    bool tryProcessShutdown(std::shared_ptr<ProcessExitEvent> pe) override;

    ~ProcessManagerImpl() override;
};
}
