// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/RunCommandWork.h"
#include "main/Application.h"
#include "process/ProcessManager.h"

namespace stellar
{

RunCommandWork::RunCommandWork(Application& app, WorkParent& parent,
                               std::string const& uniqueName, size_t maxRetries)
    : Work(app, parent, uniqueName, maxRetries)
{
}

RunCommandWork::~RunCommandWork()
{
    clearChildren();
}

void
RunCommandWork::onStart()
{
    std::string cmd, outfile;
    getCommand(cmd, outfile);
    if (!cmd.empty())
    {
        auto exit = mApp.getProcessManager().runProcess(cmd, outfile);
        exit.async_wait(callComplete());
    }
    else
    {
        scheduleSuccess();
    }
}

void
RunCommandWork::onRun()
{
    // Do nothing: we ran the command in onStart().
}
}
