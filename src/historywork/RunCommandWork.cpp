// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/RunCommandWork.h"
#include "main/Application.h"
#include "process/ProcessManager.h"
#include <Tracy.hpp>

namespace stellar
{

RunCommandWork::RunCommandWork(Application& app, std::string const& name,
                               size_t maxRetries)
    : BasicWork(app, name, maxRetries)
{
}

BasicWork::State
RunCommandWork::onRun()
{
    ZoneScoped;
    if (mDone)
    {
        return mEc ? State::WORK_FAILURE : State::WORK_SUCCESS;
    }
    else
    {
        CommandInfo commandInfo = getCommand();
        auto cmd = commandInfo.mCommand;
        auto outfile = commandInfo.mOutFile;
        if (!cmd.empty())
        {
            mExitEvent = mApp.getProcessManager().runProcess(cmd, outfile);
            auto exit = mExitEvent.lock();
            if (!exit)
            {
                return State::WORK_FAILURE;
            }

            std::weak_ptr<RunCommandWork> weak(
                std::static_pointer_cast<RunCommandWork>(shared_from_this()));
            exit->async_wait([weak](asio::error_code const& ec) {
                auto self = weak.lock();
                if (self && !self->isDone())
                {
                    self->mEc = ec;
                    self->mDone = true;
                    self->wakeUp();
                }
            });
            return State::WORK_WAITING;
        }
        else
        {
            return State::WORK_SUCCESS;
        }
    }
}

void
RunCommandWork::onReset()
{
    mDone = false;
    mEc = asio::error_code();
    mExitEvent.reset();
}

bool
RunCommandWork::onAbort()
{
    ZoneScoped;
    auto process = mExitEvent.lock();
    if (!process)
    {
        // Process is already destroyed, complete abort
        return true;
    }

    return mApp.getProcessManager().tryProcessShutdown(process);
}
}
