#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/NonCopyable.h"
#include "util/Timer.h"
#include <functional>
#include <memory>
#include <string>

namespace stellar
{

class RealTimer;

/**
 * This module exists because asio doesn't know much about subprocesses,
 * so we provide a little machinery for running subprocesses and waiting
 * on their results, intermixed with normal asio primitives.
 *
 * No facilities exist for reading or writing to the subprocess I/O ports. This
 * is strictly for "run a command, wait to see if it worked"; a glorified
 * asynchronous version of system().
 */

// Wrap a platform-specific Impl strategy that monitors process-exits in a
// helper simulating an event-notifier, via a general asio timer set to maximum
// duration. Clients can register handlers on this and they are wrapped into
// handlers on the timer, with impls that cancel the timer and pass back an
// appropriate error code when the process exits.
class ProcessExitEvent
{
    class Impl;
    std::shared_ptr<RealTimer> mTimer;
    std::shared_ptr<Impl> mImpl;
    std::shared_ptr<asio::error_code> mEc;
    ProcessExitEvent(asio::io_service& io_service);
    friend class ProcessManagerImpl;

  public:
    ~ProcessExitEvent();
    void async_wait(std::function<void(asio::error_code)> const& handler);
};

class ProcessManager : public std::enable_shared_from_this<ProcessManager>,
                       public NonMovableOrCopyable
{
  public:
    static std::shared_ptr<ProcessManager> create(Application& app);
    virtual ProcessExitEvent runProcess(std::string const& cmdLine,
                                        std::string outputFile = "") = 0;
    virtual size_t getNumRunningProcesses() = 0;
    virtual bool isShutdown() const = 0;
    virtual void shutdown() = 0;
    virtual ~ProcessManager()
    {
    }
};
}
