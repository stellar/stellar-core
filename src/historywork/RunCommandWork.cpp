// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/RunCommandWork.h"
#include "main/Application.h"
#include "process/ProcessManager.h"

namespace stellar
{

RunCommandWork::RunCommandWork(Application& app, std::function<void()> callback,
                               std::string const& name, size_t maxRetries)
    : Work(app, callback, name, maxRetries)
{
}

BasicWork::State
RunCommandWork::doWork()
{
    if (mDone)
    {
        return mEc ? BasicWork::WORK_FAILURE_RETRY : BasicWork::WORK_SUCCESS;
    }
    else
    {
        std::string cmd, outfile;
        std::tie(cmd, outfile) = getCommand();
        if (!cmd.empty())
        {
            auto exit = mApp.getProcessManager().runProcess(cmd, outfile);

            std::weak_ptr<RunCommandWork> weak(
                std::static_pointer_cast<RunCommandWork>(shared_from_this()));
            exit.async_wait([weak](asio::error_code ec) {
                auto self = weak.lock();
                if (self)
                {
                    self->mEc = ec;
                    self->mDone = true;
                    self->wakeUp();
                }
            });
            return BasicWork::WORK_WAITING;
        }
        else
        {
            return BasicWork::WORK_SUCCESS;
        }
    }
}

void
RunCommandWork::doReset()
{
    mDone = false;
    mEc = asio::error_code();
}
}
