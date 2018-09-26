// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0
#pragma once

#include "main/Application.h"

namespace stellar
{

/** BasicWork is an implementation of a finite state machine,
 * that is used for async or long-running tasks that:
 * - May depend on other Work before starting
 *  - May have parts that can run in parallel, other parts in serial
 *  - May need to be broken into steps, so as not to block the main thread
 *  - May fail and need to retry, after some delay
 *
 *  BasicWork manages all state transitions via `crankWork` as well as retries.
 *  While customers can trigger cranking and check on BasicWork's status,
 *  it is implementers responsibility to implement `onRun`, that
 *  hints the next desired state.
 */
class BasicWork : public std::enable_shared_from_this<BasicWork>,
                  private NonMovableOrCopyable
{
  public:
    static size_t const RETRY_NEVER;
    static size_t const RETRY_ONCE;
    static size_t const RETRY_A_FEW;
    static size_t const RETRY_A_LOT;
    static size_t const RETRY_FOREVER;

    enum State
    {
        WORK_RUNNING,
        WORK_SUCCESS,
        WORK_WAITING,
        WORK_FAILURE_RETRY,
        WORK_FAILURE_RAISE,
        // TODO (mlo) fatal failure seems redundant now
        WORK_FAILURE_FATAL
    };

    BasicWork(Application& app, std::string name,
              std::function<void()> callback, size_t maxRetries = RETRY_A_FEW);
    virtual ~BasicWork();

    virtual std::string getName() const;
    virtual std::string getStatus() const;
    State getState() const;
    bool isDone() const;

    virtual size_t getMaxRetries() const;
    uint64_t getRetryETA() const;

    // Main method for state transition, mostly dictated by `onRun`
    void crankWork();

  protected:
    // Implementers can override these callbacks to customize functionality
    // `onReset` is called on any important state transition, e.g. when new work
    // is added, on a retry, success and failure. This method ensures that
    // all the references (including children) are properly cleaned up for
    // safe destruction. `onRun` performs necessary work logic, and hints the
    // next state transition. `onSuccess` is called when work transitions into
    // WORK_SUCCESS state. Similarly, `onFailure*` methods are called upon
    // transitioning into appropriate failure state. An example usage would be
    // cleaning up side effects, like downloaded files in these methods. If you
    // want to force failure (and reset / retry) you can return
    // WORK_FAILURE_RETRY or WORK_FAILURE_RAISE. After a retry count is
    // passed, WORK_FAILURE_RETRY means WORK_FAILURE_RAISE anyways.
    // WORK_FAILURE_FATAL is equivalent to WORK_FAILURE_RAISE passed up in
    // the work chain - when WORK_FAILURE_FATAL is raised in one work item,
    // all work items that leaded to this one will also fail without retrying.

    virtual void onReset();
    virtual State onRun() = 0;
    virtual void onFailureRetry();
    virtual void onFailureRaise();
    virtual void onSuccess();

    void reset();

    // A helper method that implementers can use if they plan to
    // utilize WAITING state. This tells the work to return to RUNNING
    // state, and propagate the notification up to the scheduler
    // An example use of this would be RunCommandWork:
    // a timer is used to async_wait for a process to exit,
    // with a call to `wakeUp` upon completion.
    void wakeUp();

    Application& mApp;
    std::function<void()> mNotifyCallback;

  private:
    VirtualClock::duration getRetryDelay() const;
    static std::string stateName(State st);
    void setState(State s);
    void waitForRetry();

    std::string mName;
    std::unique_ptr<VirtualTimer> mRetryTimer;

    State mState{WORK_RUNNING};
    size_t mRetries{0};
    size_t mMaxRetries{RETRY_A_FEW};
    bool mRetrying{false};
};
}
