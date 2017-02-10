#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Timer.h"
#include "work/WorkParent.h"
#include <map>
#include <memory>
#include <string>

namespace stellar
{

class Application;
class WorkParent;

/** Class 'Work' (and its friends 'WorkManager' and 'WorkParent') support
 * structured dispatch of async or long-running activities that:
 *
 *  - May depend on other Work before starting
 *  - May have parts that can run in parallel, other parts in serial
 *  - May need to be broken into steps, so as not to block the main thread
 *  - May fail and need to retry, after some delay
 *
 * Formerly this was managed through ad-hoc sprinkled-through-the-code
 * copies of each of these facets of work-management. 'Work' is an attempt
 * to make those facets uniform, systematic, and out-of-the-way of the
 * logic of each piece of work.
 */

class Work : public WorkParent
{

  public:
    static size_t const RETRY_ONCE = 1;
    static size_t const RETRY_A_FEW = 5;
    static size_t const RETRY_A_LOT = 32;
    static size_t const RETRY_FOREVER = 0xffffffff;

    enum State
    {
        WORK_PENDING,
        WORK_RUNNING,
        WORK_SUCCESS,
        WORK_FAILURE_RETRY,
        WORK_FAILURE_RAISE
    };

    Work(Application& app, WorkParent& parent, std::string uniqueName,
         size_t maxRetries = RETRY_A_FEW);

    virtual ~Work();

    virtual std::string getUniqueName() const;
    virtual std::string getStatus() const;
    virtual VirtualClock::duration getRetryDelay() const;
    virtual size_t getMaxRetries() const;
    uint64_t getRetryETA() const;

    // Customize work behavior via these callbacks. onReset is called
    // before any work starts (on addition, or retry). onStart is called
    // when transitioning from WORK_PENDING -> WORK_RUNNING; onRun is
    // called when run either from PENDING state _or_ after a SUCCESS
    // callback reschedules running (see below). onFailure is only called
    // on a failure before retrying; usually you can ignore it.
    virtual void onReset();
    virtual void onStart();
    virtual void onRun();
    virtual void onFailureRetry();
    virtual void onFailureRaise();

    // onSuccess is a little different than the others: it's called on
    // WORK_SUCCESS, but it also returns the next sate desired: if you want
    // to restart or keep going, return WORK_PENDING or WORK_RUNNING
    // (respectively) from onSuccess and you'll be rescheduled to run more.
    // If you want to force failure (and reset / retry) you can return
    // WORK_FAILURE_RETRY or WORK_FAILURE_RAISE. After a retry count is
    // passed, WORK_FAILURE_RETRY means WORK_FAILURE_RAISE anyways.
    virtual State onSuccess();

    static std::string stateName(State st);
    State getState() const;
    bool isDone() const;
    void advance();
    void reset();

  protected:
    std::weak_ptr<WorkParent> mParent;
    std::string mUniqueName;
    size_t mMaxRetries{RETRY_A_FEW};
    size_t mRetries{0};
    State mState{WORK_PENDING};

    std::unique_ptr<VirtualTimer> mRetryTimer;

    std::function<void(asio::error_code const& ec)> callComplete();
    void run();
    void complete(asio::error_code const& ec);
    void scheduleComplete(asio::error_code ec = asio::error_code());
    void scheduleRetry();
    void scheduleRun();
    void
    scheduleSuccess()
    {
        scheduleComplete();
    }
    void
    scheduleFailure()
    {
        scheduleComplete(std::make_error_code(std::errc::io_error));
    }

    void setState(State s);

    void notifyParent();
    virtual void notify(std::string const& childChanged) override;
};
}
