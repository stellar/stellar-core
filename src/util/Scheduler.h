#pragma once

// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <chrono>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <string>

// This class implements a multi-queue scheduler for "actions" (deferred-work
// callbacks that some subsystem wants to run "soon" on the main thread),
// attempting to satisfy a variety of constraints and goals simultaneously:
//
//   0. Non-preemption: We have no ability to preempt actions while they're
//      running so this is a hard constraint, not just a goal.
//
//   1. Serial execution: within a queue, actions must run in the order they are
//      enqueued (or a subsequence thereof, if there are dropped actions) so
//      that clients can use queue-names to sequence logically-sequential
//      actions. Scheduling happens between queues but not within them.
//
//   2. Non-starvation: Everything enqueued (and not dropped) should run
//      eventually and the longer something waits, generally the more likely it
//      is to run.
//
//   3. Fairness: time given to each queue should be roughly equal, over time.
//
//   4. Load-shedding and back-pressure: we want to be able to define a load
//      limit (in terms of worst-case time actions are delayed in the queue)
//      beyond which we consider the system to be "overloaded" and both shed
//      load where we can (dropping non-essential actions) and exert
//      backpressure on our called (eg. by having them throttle IO that
//      ultimately drives queue growth).
//
//   5. Simplicity: clients of the scheduler shouldn't need to adjust a lot of
//      knobs, and the implementation should be as simple as possible and
//      exhibit as fixed a behaviour as possible. We don't want surprises in
//      dynamics.
//
//   6. Non-anticipation: many scheduling algorithms require more information
//      than we have, or are so-called "anticipation" algorithms that need to
//      know (or guess) the size or duration of the next action. We certainly
//      don't know these, and while we _could_ try to estimate them, the
//      algorithms that need anticipation can go wrong if fed bad estimates;
//      we'd prefer a non-anticipation (or "blind") approach that lacks this
//      risk.
//
// Given these goals and constraints, our current best guess is a lightly
// customized algorithm in a family called FB ("foreground-background" or
// "feedback") or LAS ("least attained service") or SET ("shortest elapsed
// time").
//
// For an idea with so many names, the algorithm is utterly simple: each queue
// tracks the accumulated runtime of all actions it has run, and on each step we
// run the next action in the queue with the lowest accumulated runtime (the
// queues themselves are therefore stored in an outer priority queue to enable
// quick retrieval of the next lowest queue).
//
// This has a few interesting properties:
//
//   - A low-frequency action (eg. a ledger close) will usually be scheduled
//     immediately, as it has built up some "credit" in its queue in the form of
//     zero new runtime in the period since its last run, lowering its
//     accumulation relative to other queues.
//
//   - A continuously-rescheduled multipart action (eg. bucket-apply or
//     catchup-replay) will quickly consume any "credit" it might have and be
//     throttled back to an equal time-share with other queues: since it spent a
//     long time on-CPU it will have to wait at least until everyone else has
//     had a similar amount of time before going again.
//
//   - If a very-short-duration action occurs it has little affect on anything
//     else, either its own queue or others, in the relative scheduling order. A
//     queue that's got lots of very-small actions (eg. just issuing a pile of
//     async IOs or writing to in-memory buffers) may run them _all_ before
//     anyone else gets to go, but that's ok precisely because they're very
//     small actions. The scheduler will shift to other queues exactly when a
//     queue uses up a _noticeable amount of time_ relative to others.
//
// This is an old algorithm that was not used for a long time out of fear that
// it would starve long-duration actions; but it's received renewed study in
// recent years based on the observation that such starvation only occurs in
// certain theoretically-tidy but practically-rare distributions of action
// durations, and the distributions that occur in reality behave quite well
// under it.
//
// The customizations we make are minor:
//
//   - We put a floor on the cumulative durations; low cumulative durations
//     represent a form of "credit" that a queue might use in a burst if it were
//     to be suddenly full of ready actions, or continuously-reschedule itself,
//     so we make sure no queue can have less than some (steadily rising) floor.
//
//   - We record the enqueue time and "droppability" of an action, to allow us
//     to measure load level and perform load shedding.

namespace stellar
{

class VirtualClock;

class Scheduler
{
  public:
    using Action = std::function<void()>;
    enum class ActionType
    {
        NORMAL_ACTION,
        DROPPABLE_ACTION
    };

    struct Stats
    {
        size_t mActionsEnqueued{0};
        size_t mActionsDequeued{0};
        size_t mActionsDroppedDueToOverload{0};
        size_t mQueuesActivatedFromFresh{0};
        size_t mQueuesActivatedFromIdle{0};
        size_t mQueuesSuspended{0};
    };

  private:
    class ActionQueue;
    using Qptr = std::shared_ptr<ActionQueue>;

    // Stores all ActionQueues by name+type, either runnable or idle.
    std::map<std::pair<std::string, ActionType>, Qptr> mAllActionQueues;

    // Stores the Runnable ActionQueues, with top() being the ActionQueue with
    // the least total service time. An ActionQueue is "runnable" if it is
    // nonempty; empty ActionQueues are considered "idle" and are tracked
    // in the mIdleActionQueues member below.
    std::priority_queue<Qptr, std::vector<Qptr>,
                        std::function<bool(Qptr, Qptr)>>
        mRunnableActionQueues;

    Stats mStats;

    // Clock we get time from.
    VirtualClock& mClock;

    // This "latency window" serves 3 distinct purposes simultaneously.
    //
    // Theoretically they could be 3 separate knobs but in practice they
    // all seem both related to one another and roughly the same order
    // of magnitude so we use a single number for all 3 presently:
    //
    //  1. The time-delta subtracted from the observed maximum totalService of
    //     any queue in order to calculate a minimum totalSerice "floor", that
    //     we always advance queues' totalService values to when we run
    //     them. One way to think of this is as a latency cap -- the longest we
    //     want to let one queue that's built up a bunch of "credit" by being
    //     lightly-loaded monopolize scheduling if it's suddenly full of work.
    //
    //  2. The maximum duration between an action's enqueue and dequeue times
    //     beyond which we consider the queue "overloaded" and begin providing
    //     backpressure / load-shedding. One way to think of this is "the
    //     longest tolerable response-delay", which if you squint is similar
    //     to the way of thinking about purpose #1 above: a latency cap.
    //
    //  3. The maximum duration a queue can be idle before we forget about it,
    //     reclaiming its memory. This is even more-obviously related to purpose
    //     #1 above: if an idle queue were made runnable after a time greater
    //     than this duration, it'd have its totalService value set to the
    //     totalService floor, just as it would if it were forgotten entirely
    //     and remade anew.

    std::chrono::nanoseconds const mLatencyWindow;

    // Largest totalService seen in any queue. This number will continuously
    // advance as queues are serviced; it exists to serve as the upper limit
    // of the window, from which mTotalServiceWindow is subtracted to derive
    // the lower limit.
    std::chrono::nanoseconds mMaxTotalService{0};

    // Sum of sizes of all the active queues. Maintained as items are enqueued
    // or run.
    size_t mSize{0};

    void trimSingleActionQueue(Qptr q,
                               std::chrono::steady_clock::time_point now);
    void trimIdleActionQueues(std::chrono::steady_clock::time_point now);

    // List of ActionQueues that are currently idle. Idle ActionQueues maintain
    // a list<Qptr>::iterator pointing to their own position in this list, which
    // can be used to make them runnable at any time. Idled ActionQueues are
    // placed at the front of this list and expired (if they are too old) from
    // the back of the list, where they've been idle the longest.
    std::list<Qptr> mIdleActionQueues;

    // transitions overloaded state
    void setOverloaded(bool overloaded);

    // records the time the scheduler transitioned to the overloaded or max if
    // not
    std::chrono::steady_clock::time_point mOverloadedStart;

    // Records the currently-executing action type, or NORMAL_ACTION when no
    // action is running. This can be retrieved through currentActionType().
    ActionType mCurrentActionType{ActionType::NORMAL_ACTION};

  public:
    Scheduler(VirtualClock& clock, std::chrono::nanoseconds latencyWindow);

    // Adds an action to the named ActionQueue with a given type.
    void enqueue(std::string&& name, Action&& action, ActionType type);

    // Runs 0 or 1 action from the next ActionQueue in the queue-of-queues.
    size_t runOne();

    // Return the ActionType of the currently-executing action; if no action
    // is currently running, return NORMAL_ACTION.
    ActionType currentActionType() const;

    // Returns how long ActionQueues have been overloaded (0 means not
    // overloaded)
    std::chrono::seconds getOverloadedDuration() const;

    size_t
    size() const
    {
        return mSize;
    }

    std::chrono::nanoseconds
    maxTotalService() const
    {
        return mMaxTotalService;
    }

    Stats const&
    stats() const
    {
        return mStats;
    }

#ifdef BUILD_TESTS
    // Testing interface
    Qptr getExistingQueue(std::string const& name, ActionType type) const;
    std::string const& nextQueueToRun() const;
    std::chrono::nanoseconds
    totalService(std::string const& q,
                 ActionType type = ActionType::NORMAL_ACTION) const;
    size_t queueLength(std::string const& q,
                       ActionType type = ActionType::NORMAL_ACTION) const;
#endif
};
}
