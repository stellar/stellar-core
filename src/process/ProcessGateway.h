#ifndef __PROCESSGATEWAY__
#define __PROCESSGATEWAY__

#include "util/Timer.h"
#include <memory>
#include <string>

namespace stellar
{

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
    std::shared_ptr<Timer> mTimer;
    std::shared_ptr<Impl> mImpl;
    std::shared_ptr<asio::error_code> mEc;
    ProcessExitEvent(asio::io_service& io_service);
    friend class ProcessMaster;
public:
    ~ProcessExitEvent();
    template <typename WaitHandler>
    void async_wait(ASIO_MOVE_ARG(WaitHandler) handler)
    {
        // Unfortunately when you cancel a timer, asio delivers
        // asio::error::operation_aborted to all the waiters, even if you pass a
        // different error_code to the cancel() call. So we have to route the
        // _actual_ process-exit condition through _another_ variable shared
        // between ProcessExitEvent and the per-platform handlers.
        auto ec = mEc;
        mTimer->async_wait(
            bind([ec](WaitHandler &handler)
                 {
                     handler(*ec);
                 }, std::move(handler)));
    }
};


class ProcessGateway
{
public:
    virtual ProcessExitEvent runProcess(std::string const& cmdLine) = 0;
};


}

#endif
