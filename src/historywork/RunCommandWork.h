#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "work/Work.h"

namespace stellar
{

// This subclass exists for two reasons: first, to factor out a little code
// around running commands, and second to ensure that command-running
// happens from onStart rather than onRun, and that onRun is an empty
// method; this way we only run a command _once_ (when it's first
// scheduled) rather than repeatedly (racing with other copies of itself)
// when rescheduled.
class RunCommandWork : public Work
{
    virtual void getCommand(std::string& cmdLine, std::string& outFile) = 0;

  public:
    RunCommandWork(Application& app, WorkParent& parent,
                   std::string const& uniqueName,
                   size_t maxRetries = Work::RETRY_A_FEW);
    ~RunCommandWork();
    void onStart() override;
    void onRun() override;
};
}
