#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

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
