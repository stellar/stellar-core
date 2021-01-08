// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0
#pragma once

#include "main/Application.h"
#include <set>

namespace stellar
{

class Application;

/**
 * BasicWork is an implementation of a finite state machine, that is used
 * for async or long-running tasks that:
 *
 *   - May need to be broken into steps, so as not to block the main
 *     thread.
 *
 *   - May fail and need to retry, after some delay.
 *
 *   - May be uniformly, actively aborted while running.
 *
 *  BasicWork manages all state transitions via `crankWork` and `setState`.
 *  It also supports a retry mechanism internal to BasicWork. While
 *  customers can trigger cranking, check on BasicWork's status and/or call
 *  `shutdown`, it is implementers' responsibility to implement `onRun`,
 *  that hints the next desired state, and `onAbort`, that cancels ongoing
 *  work.
 *
 *  The implementer's return value from `onRun` is the `State` type, which
 *  has only 5 values (WORK_{RUNNING,WAITING,SUCCESS,FAILURE,ABORTED}),
 *  each of which has a corresponding InternalState value.
 *
 *  BasicWork _internally_ uses the `InternalState` type, which has 8 values
 *  (the values of the State type, plus PENDING, RETRYING and ABORTING). It
 *  uses these to provide central treatments of (re)initialization, retrying
 *  and aborting.
 *
 *  The legal state transitions (and event edges) are as follows:
 *
 *
 *       +--------------------------startWork--------------------------+
 *       |                                                             |
 *       v                                               +----------+  |
 *  +---------+                                          |*SUCCESS* |  |
 *  | PENDING |>-+                  +-onSuccess--------->|  (done)  |>-+
 *  +---------+  |                  |                    +----------+  |
 *       v       |                  |                    +----------+  |
 *       |    onReset,              |                    |*FAILURE* |  |
 *       |   retries=0   +-------+  +-onFailureRaise,--->|  (done)  |>-+
 *       |       |       |       |  |     onReset        +----------+  |
 *       |       |       v       |  |                    +----------+  |
 *       |       |  +---------+  |  |                    |*ABORTED* |  |
 *       |       +->| RUNNING |>-+--+-----------------+  |  (done)  |>-+
 *       |       |  +---------+                       |  +----------+
 *       |       |       v                            |        ^
 *       |       |       |                            |        |
 *       |       |       +-onFailureRetry,--+     shutdown     |
 *       |       |       |     onReset      |         |        |
 *       |       |       |                  v         |        |
 *       |       |       |            +-----------+   |        |
 *       |       |       |            | RETRYING  |   |        |
 *       |    wakeUp,    |            +-----------+   |        |
 *       |    notify     |                  v         v    onAbort
 *       |       |       |                  |         |    (true),
 *       |       |       +--waitForRetry----+         |    onReset
 *       |       |       |                            |        |
 *       |       |       v                            |        |
 *       |       | +-----------+                      |        |
 *       |       | |  WAITING  |>------shutdown-------+        |
 *       |       | +-----------+                      |        |
 *       |       |       v                            |        |
 *       |       |       |                            v        |
 *       |       +-------+                      +-----------+  |
 *       +----------------------shutdown-----+->| ABORTING  |>-+
 *                                           |  +-----------+
 *                                           |        v
 *                                           |        |
 *                                           +onAbort-+
 *                                            (false)
 *
 *
 *  BasicWork is a bulding block of task execution; Other notable works
 * available for implementers:
 *  - Work: BasicWork that allows adding and managing children.
 *  - WorkScheduler: Work that controls scheduling the hierarchy on the IO
 * service, note that children are scheduled in the round-robin fashion.
 *  - BatchWork: Work that allows scheduling children to run in parallel in
 * _batches_
 *  - WorkSequence: BasicWork that allows sequential execution of children
 * works.
 */

class BasicWork : public std::enable_shared_from_this<BasicWork>,
                  private NonMovableOrCopyable
{
  public:
    static size_t const RETRY_NEVER;
    static size_t const RETRY_ONCE;
    static size_t const RETRY_A_FEW;
    static size_t const RETRY_A_LOT;

    // Publicly exposed state of work
    enum class State
    {
        // Work has been created and is currently in progress.
        // Implementers return RUNNING when work needs to be scheduled to run
        // more.
        WORK_RUNNING,
        // Work should not be scheduled to run, as it's waiting
        // for some event (process exit, retry timeout, etc)
        // Note that it is implementers' responsibility then to correctly
        // use `wakeUp` to exit WAITING state.
        WORK_WAITING,
        // Terminal aborted state
        WORK_ABORTED,
        // Terminal successful state
        WORK_SUCCESS,
        // Terminal unsuccessful state. When this state is
        // returned, depending on the Work's retry strategy, either a retry will
        // be scheduled or work will cease execution.
        WORK_FAILURE
    };

    BasicWork(Application& app, std::string name, size_t maxRetries);
    virtual ~BasicWork();

    std::string const& getName() const;
    virtual std::string getStatus() const;
    State getState() const;
    bool isDone() const;

    // Main method for state transition, mostly dictated by `onRun`
    void crankWork();

    // Reset work to its initial state (only if work is in terminal state)
    // Additionally, assign a notification callback to be used once work
    // makes important state transitions. For example, in the context of `Work`,
    // such callback should be passed into child, so that it can wake its parent
    // up once finished.
    void startWork(std::function<void()> notificationCallback);

    // Prepare work for destruction. Implementers overriding this method
    // are expected to call it on all work they might have created.
    // Note `BasicWork::shutdown` must also be called, as it changes the
    // internal state of work.
    virtual void shutdown();

    bool
    isAborting() const
    {
        return mState == InternalState::ABORTING;
    }

  protected:
    // Implementers _must_ override `onRun`, as it performs necessary work
    // logic, and hints the next state transition.
    virtual State onRun() = 0;

    // Implementers _must_ decide what they want to do when asked to
    // shutdown. Abort is not just destruction or cleanup: it's active
    // intervention in otherwise-running work, to _make it stop_. Some
    // examples would be aborting children, then aborting self
    // (e.g. `Work`) or killing a process that it's managing
    // (`RunCommandWork`)
    virtual bool onAbort() = 0;

    // Implementers may (but need not) override these callbacks to
    // customize functionality. `onReset` is expected to restore work's
    // state to its initial condition; this includes cleaning up side
    // effects (e.g. produced files), restoring member variables
    // etc. `onSuccess` is called when work transitions into WORK_SUCCESS
    // state. Similarly, `onFailure*` methods are called upon transitioning
    // into appropriate failure states. Note that implementers return a
    // general WORK_FAILURE value from `onRun`, and it is the base class
    // here `BasicWork` that translates that general signal of failure into
    // a decision to retry or fail permanently, using its retry counter and
    // schedule.
    virtual void onReset();
    virtual void onFailureRetry();
    virtual void onFailureRaise();
    virtual void onSuccess();

    // A helper method that implementers can use if they plan to utilize
    // WAITING state. This tells the work to return to RUNNING state, and
    // propagate the notification up to the scheduler. An example use of
    // this would be RunCommandWork: a timer is used to async_wait for a
    // process to exit, with a call to `wakeUp` upon completion.
    virtual void wakeUp(std::function<void()> innerCallback = nullptr);

    // Default wakeUp callback that implementers can use
    std::function<void()>
    wakeSelfUpCallback(std::function<void()> innerCallback = nullptr);

    // To yield, setup a timer. Upon expiration, work will wake itself up and
    // continue execution. Use this function before transitioning into
    // WORK_WAITING state. Note: this function is idempotent
    void setupWaitingCallback(std::chrono::milliseconds wakeUpIn);

    Application& mApp;

  private:
    // Internally, there are a few more states that aid with
    // state transitions.
    enum class InternalState
    {
        // Work has been created but hasn't started yet.
        PENDING,
        RUNNING,
        WAITING,
        // Work is shutting down. During this stage, Work is prevented from
        // making any furhter progress. This means that any attempts to call
        // wake, add child work or invoke a callback will result in a no-op.
        ABORTING,
        ABORTED,
        // `onRun` returned WORK_FAILURE. If there are retries left, go into
        // RETRYING state.
        RETRYING,
        SUCCESS,
        FAILURE
    };
    using Transition = std::pair<InternalState, InternalState>;

    // Reset work to its original state, typically done when transitioning
    // *from* a terminal state, retrying or aborting.
    void reset();

    VirtualClock::duration getRetryDelay() const;
    void setState(InternalState s);
    void waitForRetry();
    InternalState getInternalState(State s) const;
    void assertValidTransition(Transition const& t) const;
    static std::string stateName(InternalState st);
    uint64_t getRetryETA() const;
    void resetWaitingTimer();

    std::function<void()> mNotifyCallback;
    std::string const mName;
    std::unique_ptr<VirtualTimer> mRetryTimer;
    std::unique_ptr<VirtualTimer> mWaitingTimer;

    InternalState mState{InternalState::PENDING};
    size_t mRetries{0};
    size_t const mMaxRetries{RETRY_A_FEW};

    // Legal and allowed state transitions in work state machine
    static std::set<Transition> const ALLOWED_TRANSITIONS;
};
}