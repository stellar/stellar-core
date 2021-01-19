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
    ProcessExitEvent(asio::io_context& io_context);
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
    virtual std::weak_ptr<ProcessExitEvent>
    runProcess(std::string const& cmdLine, std::string outputFile) = 0;

    // Return the number or processes we started and have not yet seen exits
    // for, _excluding_ those we're attempting to shut down / are shortly
    // expecting to see an exit for (which are likely already dead, just not yet
    // reaped). This number will decrement as soon as the associated
    // ProcessExitEvent is fired.
    virtual size_t getNumRunningProcesses() = 0;

    // Return the number of processes we started and have not yet seen exits
    // for, _including_ those we're in the process of shutting down. This number
    // may remain higher than expected for a while after a ProcessExitEvent is
    // fired.
    virtual size_t getNumRunningOrShuttingDownProcesses() = 0;

    virtual bool isShutdown() const = 0;
    virtual void shutdown() = 0;

    // Synchronously cancels the provided ProcessExitEvent (firing its event
    // handler with ABORT_ERROR_CODE) and attempts to terminate the associated
    // process if it is running. Returns true if the attempt was successful --
    // in the sense of succesfully sending a SIGTERM for example -- though this
    // does not guarantee that the process _has exited_ yet.
    //
    // Since the event is cancelled, there is no further way for clients to be
    // certain the process has exited using this interface. If a process that
    // has been sent a termination signal does _not_ exit, however, it will
    // remain tracked by the ProcessManager, and be forcibly killed when
    // the ProcessManager is destructed.
    virtual bool tryProcessShutdown(std::shared_ptr<ProcessExitEvent> pe) = 0;
    virtual ~ProcessManager()
    {
    }
};
}
