#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "process/ProcessManager.h"
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
    // receipt of SIGCHLD.
    static std::recursive_mutex gImplsMutex;
    static std::map<int, std::shared_ptr<ProcessExitEvent::Impl>> gImpls;

    Application& mApp;

    // These are only used on POSIX, but they're harmless here.
    asio::signal_set mSigChild;
    void startSignalWait();
    void handleSignalWait();

    medida::Counter& mImplsSize;

  public:
    ProcessManagerImpl(Application& app);
    ProcessExitEvent runProcess(std::string const& cmdLine,
                                std::string outFile = "");
};
}
