#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "process/ProcessManager.h"
#include "work/Work.h"

namespace stellar
{
struct CommandInfo
{
    std::string mCommand;
    std::string mOutFile;
};

/**
 * This class helps run various commands, that require
 * process spawning. This work is not scheduled while it's
 * waiting for a process to exit, and wakes up when it's ready
 * to be scheduled again.
 */
class RunCommandWork : public BasicWork
{
    bool mDone{false};
    asio::error_code mEc;
    virtual CommandInfo getCommand() = 0;
    std::weak_ptr<ProcessExitEvent> mExitEvent;

  public:
    RunCommandWork(Application& app, std::string const& name,
                   size_t maxRetries = BasicWork::RETRY_A_FEW);
    ~RunCommandWork() = default;

  protected:
    void onReset() override;
    BasicWork::State onRun() override;
    bool onAbort() override;
};
}
