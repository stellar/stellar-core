---
name: subsystem-summary-of-work
description: "read this skill for a token-efficient summary of the work subsystem"
---

# Work Subsystem — Technical Summary

## Overview

The work subsystem provides a cooperative, single-threaded, asynchronous task-execution framework for stellar-core. It implements a finite state machine (FSM) model where long-running or multi-step tasks are broken into small "cranks" that execute on the main thread without blocking. The framework supports hierarchical task trees, retry logic with exponential backoff, sequential and parallel execution, conditional gating, and orderly abort/shutdown.

All work executes on the main thread via the application's IO service. The subsystem is **not** thread-safe; the only exception is spawning independent background I/O (file reads, downloads) that post results back to the main thread.

## Key Files

- **BasicWork.h / BasicWork.cpp** — Base FSM class; state machine, retry logic, crank mechanism.
- **Work.h / Work.cpp** — Extends BasicWork with child-management (hierarchical work trees).
- **WorkScheduler.h / WorkScheduler.cpp** — Top-level scheduler; posts cranks to the IO service.
- **WorkSequence.h / WorkSequence.cpp** — Sequential execution of an ordered vector of BasicWork items.
- **BatchWork.h / BatchWork.cpp** — Parallel batched execution with throttling.
- **ConditionalWork.h / ConditionalWork.cpp** — Gates a work item on an arbitrary monotonic condition.
- **WorkWithCallback.h / WorkWithCallback.cpp** — Wraps a simple callback as a one-shot BasicWork.

---

## Class Hierarchy

```
BasicWork                          (base FSM, abstract)
├── Work                           (adds child management, abstract)
│   ├── WorkScheduler              (top-level scheduler, singleton-like)
│   └── BatchWork                  (parallel batched execution, abstract)
├── WorkSequence                   (sequential execution of a vector)
├── ConditionalWork                (condition-gated delegation)
└── WorkWithCallback               (one-shot callback wrapper)
```

All classes inherit `std::enable_shared_from_this<BasicWork>` and `NonMovableOrCopyable`. Ownership is via `std::shared_ptr`.

---

## Key Classes and Data Structures

### `BasicWork`

The foundational finite state machine. Every work unit in stellar-core derives from this.

**Public State enum (`BasicWork::State`):**
- `WORK_RUNNING` — Work needs more cranks to make progress.
- `WORK_WAITING` — Work is idle, waiting for an external event (timer, process exit, child completion).
- `WORK_SUCCESS` — Terminal: work completed successfully.
- `WORK_FAILURE` — Terminal: work failed (may trigger retry internally).
- `WORK_ABORTED` — Terminal: work was actively aborted.

**Internal State enum (`BasicWork::InternalState`, private):**
Extends the public state with three additional values used to drive internal transitions:
- `PENDING` — Created but not yet started.
- `RETRYING` — `onRun` returned FAILURE but retries remain; scheduling a retry timer.
- `ABORTING` — Shutdown requested, actively aborting.

**Key Members:**
- `mApp` (`Application&`) — Back-reference to the application.
- `mName` (`std::string const`) — Unique human-readable name.
- `mState` (`std::atomic<InternalState>`) — Current FSM state. Atomic only for safe const-reads from background threads; all mutations happen on main thread.
- `mRetries` / `mMaxRetries` (`size_t`) — Current retry count and maximum allowed.
- `mRetryTimer` (`std::unique_ptr<VirtualTimer>`) — Timer for exponential-backoff retries.
- `mWaitingTimer` (`std::unique_ptr<VirtualTimer>`) — Timer for waking up from WAITING state.
- `mNotifyCallback` (`std::function<void()>`) — Callback to notify parent/scheduler of state changes.
- `ALLOWED_TRANSITIONS` (`std::set<Transition>`, static) — Whitelist of legal `(from, to)` state pairs.

**Retry Constants:**
- `RETRY_NEVER = 0`, `RETRY_ONCE = 1`, `RETRY_A_FEW = 5`, `RETRY_A_LOT = 32`.

**Key Methods:**

| Method | Description |
|--------|-------------|
| `crankWork()` | Main entry point for advancing the FSM. If ABORTING, calls `onAbort()`; otherwise calls `onRun()` and transitions to the returned state. |
| `startWork(notificationCallback)` | Initializes work from PENDING state, sets notification callback, transitions to RUNNING, resets retry counter. |
| `shutdown()` | Transitions to ABORTING state (if not already done). Cancels waiting timer. |
| `isDone()` | Returns true if in SUCCESS, FAILURE, or ABORTED state. |
| `getState()` | Maps InternalState to the public State enum. PENDING/ABORTING map to WORK_RUNNING; RETRYING maps to WORK_WAITING. |
| `setState(s)` | Validates transition legality, triggers lifecycle callbacks (`onSuccess`, `onFailureRaise`, `onFailureRetry`), updates state. Automatically converts FAILURE→RETRYING if retries remain. |
| `wakeUp(innerCallback)` | Transitions from WAITING→RUNNING, executes optional inner callback, then fires `mNotifyCallback` to propagate upward. |
| `wakeSelfUpCallback(innerCallback)` | Returns a `std::function<void()>` closure (capturing weak_ptr to self) that calls `wakeUp`. Used to wire child→parent notification. |
| `setupWaitingCallback(duration)` | Creates a timer that will call `wakeUp` after the given duration. Idempotent—no-ops if timer already set. Used before returning WORK_WAITING. |
| `waitForRetry()` | Creates `mRetryTimer` with exponential backoff delay, transitions to WAITING. On timer expiry, increments retry count and calls `wakeUp`. |
| `reset()` | Cancels retry timer, calls `onReset()`. Called on `PENDING→RUNNING`, `RETRYING`, `FAILURE`, and `ABORTED` transitions. |

**Pure Virtual (Implementer must override):**
- `onRun()` → Returns desired next `State`. Contains the actual work logic.
- `onAbort()` → Returns `true` when abort is complete, `false` if still aborting.

**Virtual Lifecycle Callbacks (optional overrides):**
- `onReset()` — Restore work to initial state, clean up side effects.
- `onSuccess()` — Called on transition to SUCCESS.
- `onFailureRetry()` — Called when transitioning to RETRYING.
- `onFailureRaise()` — Called when transitioning to terminal FAILURE.

---

### `Work` (extends `BasicWork`)

Adds the ability to manage a set of child work items, forming a tree. This enables supervisor-style patterns where a parent dispatches sub-tasks and aggregates results.

**Key Members:**
- `mChildren` (`std::list<std::shared_ptr<BasicWork>>`) — Currently active children.
- `mNextChild` (list iterator) — Round-robin pointer for fair scheduling.
- `mDoneChildren` / `mTotalChildren` (`size_t`) — Counters for status reporting.
- `mAbortChildrenButNotSelf` (`bool`) — Flag set when a child fails and remaining children must be aborted before the parent can report failure.

**Key Methods:**

| Method | Description |
|--------|-------------|
| `addWork<T>(args...)` | Template: creates a `shared_ptr<T>` child, wires its notification callback to `wakeSelfUpCallback`, starts it. |
| `addWorkWithCallback<T>(cb, args...)` | Like `addWork` but with an additional callback run on child notification. |
| `addWork(cb, child)` | Non-template: adds a pre-constructed child, starts it, fires initial notification. |
| `onRun()` (final) | Round-robin cranks the next RUNNING child via `yieldNextRunningChild()`. When no runnable children remain, calls `doWork()`. |
| `onAbort()` (final) | Cranks aborting children in round-robin until all are done. |
| `onReset()` (final) | Clears children, resets `mAbortChildrenButNotSelf`, calls `doReset()`. |
| `yieldNextRunningChild()` | Iterates from `mNextChild`; returns first RUNNING child. Removes done children from the list as it goes. Wraps around at end. Returns nullptr if none. |
| `checkChildrenStatus()` | Aggregates children states: all-success→SUCCESS, any-failed→FAILURE, none-running→WAITING, else RUNNING. |
| `shutdown()` | Shuts down all non-done children, then calls `BasicWork::shutdown()`. |
| `clearChildren()` | Asserts all children done, clears list, resets iterator. |

**Pure Virtual:**
- `doWork()` — Implementers define local work at this tree node (spawn more children, inspect existing ones, or do local computation).

**Virtual:**
- `doReset()` — Additional cleanup for subclasses on reset.

**Important Behavioral Detail:** When `doWork()` returns `WORK_FAILURE` but not all children are done, the parent sets `mAbortChildrenButNotSelf = true`, shuts down children, and keeps returning `WORK_RUNNING` until all children are aborted. Only then does it report FAILURE. This ensures clean shutdown of the subtree before failure propagation.

**`WorkUtils` namespace:** Free functions operating on `std::list<shared_ptr<BasicWork>>`:
- `allSuccessful()`, `anyFailed()`, `anyRunning()`, `getWorkStatus()`.

---

### `WorkScheduler` (extends `Work`)

The top-level work scheduler. One instance per application, created via `WorkScheduler::create(app)`. It bridges the work subsystem to the application's IO service by posting cranks.

**Key Members:**
- `mScheduled` (`bool`) — Guard to prevent double-scheduling on the IO service.
- `mTriggerTimer` (`VirtualTimer`) — Periodic trigger (unused in current scheduling approach; cranks are event-driven).
- `TRIGGER_PERIOD` (`50ms`, static) — Minimum crank interval.

**Key Methods:**

| Method | Description |
|--------|-------------|
| `create(app)` | Factory: constructs a `WorkScheduler`, calls `startWork(nullptr)` and an initial `crankWork()`. Returns `shared_ptr`. |
| `scheduleWork<T>(args...)` | Template: creates and schedules a child work. Wires a callback that calls `scheduleOne()` to ensure continued cranking. Returns the child (or nullptr if aborting/done). |
| `executeWork<T>(args...)` | Synchronous wrapper: calls `scheduleWork`, then busy-loops `clock.crank(true)` until the work is done. Used for blocking tasks (e.g., command-line operations). |
| `scheduleOne(weak)` | Static: posts a main-thread callback that loops calling `crankWork()` as long as state is RUNNING and the clock hasn't yielded. Reschedules itself if still RUNNING. Prevents double-scheduling via `mScheduled` flag. |
| `doWork()` | Returns RUNNING if any child is running, WAITING otherwise. |
| `shutdown()` | Calls `Work::shutdown()`, then schedules one more crank to drain aborting children. |

**Scheduling Model:** The scheduler uses cooperative, event-driven scheduling. When a child is added via `scheduleWork`, a callback is wired that calls `scheduleOne()`. This posts a closure to the application's main thread IO service. Inside that closure, `crankWork()` is called in a loop until either the scheduler is no longer RUNNING or the clock indicates it should yield (`shouldYield()`). If still RUNNING after the loop, `scheduleOne` is called again to reschedule. This ensures fair interleaving with other main-thread work (ledger closing, overlay, etc.).

---

### `WorkSequence` (extends `BasicWork`)

Executes a vector of `BasicWork` items in strict sequential order. Each item is started as the previous one completes.

**Key Members:**
- `mSequenceOfWork` (`std::vector<std::shared_ptr<BasicWork>>`) — Ordered list of work items.
- `mNextInSequence` (vector iterator) — Points to the next work to start.
- `mCurrentExecuting` (`std::shared_ptr<BasicWork>`) — The currently active work item.
- `mStopAtFirstFailure` (`bool const`) — If true (default), stops the sequence on first failure. If false, continues and aggregates final status.

**Key Methods:**

| Method | Description |
|--------|-------------|
| `onRun()` | If at end of sequence, returns aggregated status. Otherwise, starts the next item if needed, cranks it, and advances on success (or failure if `!mStopAtFirstFailure`). |
| `onAbort()` | Cranks the currently executing item until it finishes aborting. |
| `onReset()` | Resets iterator to beginning, clears `mCurrentExecuting`. |
| `shutdown()` | Shuts down `mCurrentExecuting` if any, then calls `BasicWork::shutdown()`. |

**Note:** WorkSequence does NOT inherit from Work; it inherits directly from BasicWork. It manages its children vector directly rather than using Work's child-management infrastructure.

---

### `BatchWork` (extends `Work`)

Runs work items in parallel batches, throttled by `MAX_CONCURRENT_SUBPROCESSES` from the application config. Subclasses supply iteration methods to generate work.

**Key Members:**
- `mBatch` (`std::map<std::string, std::shared_ptr<BasicWork>>`) — Tracks currently active batch items by name.

**Key Methods:**

| Method | Description |
|--------|-------------|
| `doWork()` | Checks for child failures (→FAILURE). Cleans up successful children from `mBatch`. Calls `addMoreWorkIfNeeded()`. Returns aggregated status. |
| `addMoreWorkIfNeeded()` | While `mBatch.size() < MAX_CONCURRENT_SUBPROCESSES` and `hasNext()`, calls `yieldMoreWork()` and adds it via `Work::addWork`. |
| `doReset()` | Clears `mBatch`, calls `resetIter()`. |

**Pure Virtual (subclass must implement):**
- `hasNext()` — Returns true if more work items are available.
- `yieldMoreWork()` — Returns the next `shared_ptr<BasicWork>` to execute.
- `resetIter()` — Resets the subclass's iteration state.

**Throttling:** The batch size is bounded by `mApp.getConfig().MAX_CONCURRENT_SUBPROCESSES`. BatchWork itself never retries (`RETRY_NEVER`).

---

### `ConditionalWork` (extends `BasicWork`)

Gates the execution of a wrapped `BasicWork` item on a monotonic condition function. Polls the condition periodically until it returns true, then delegates to the conditioned work.

**Key Members:**
- `mCondition` (`ConditionFn` = `std::function<bool(Application&)>`) — The gating condition. Must be monotonic (once true, always true). Set to nullptr after condition is met.
- `mConditionedWork` (`std::shared_ptr<BasicWork>`) — The wrapped work item.
- `mSleepDelay` (`std::chrono::milliseconds`, default 100ms) — Polling interval while condition is false.
- `mWorkStarted` (`bool`) — Whether the conditioned work has been started.

**Key Methods:**

| Method | Description |
|--------|-------------|
| `onRun()` | If work started, cranks it and returns its state. Otherwise, checks condition: if false, sets up a waiting timer and returns WAITING; if true, starts the conditioned work and recurses. |
| `onAbort()` | If work started and not done, cranks it; returns false. Otherwise returns true. |
| `onReset()` | Resets `mWorkStarted` to false. |
| `shutdown()` | Shuts down conditioned work if started, then calls `BasicWork::shutdown()`. |

**`ConditionFn` contract:** Must be monotonic — once it returns true, it must always return true thereafter. The function is deleted (set to nullptr) after the condition is first satisfied.

**Usage Pattern:** ConditionalWork enables adding sequential dependency edges within otherwise-parallel execution. For example, in `DownloadApplyTxsWork` (a BatchWork), each yielded WorkSequence contains a download step followed by a ConditionalWork wrapping an apply step, where the condition checks that the previous sequence's apply step has completed.

---

### `WorkWithCallback` (extends `BasicWork`)

A simple one-shot work that wraps a `std::function<bool(Application&)>`. Returns SUCCESS if the callback returns true, FAILURE otherwise. Catches `std::runtime_error` exceptions from the callback and maps them to FAILURE.

**Key Members:**
- `mCallback` (`std::function<bool(Application& app)> const`) — The callback to execute.

**Key Methods:**
- `onRun()` — Calls `mCallback(mApp)`. Returns SUCCESS on true, FAILURE on false or exception.
- `onAbort()` — Returns true immediately (nothing to abort).

Never retries (`RETRY_NEVER`).

---

## State Machine and Lifecycle

### State Transition Diagram

The FSM has 8 internal states with 16 legal transitions (defined in `ALLOWED_TRANSITIONS`):

```
PENDING ──[startWork]──► RUNNING ──[onRun→SUCCESS]──► SUCCESS
    ▲                      │  ▲                          │
    │                      │  │                          │
    │                      │  └──[onRun→RUNNING]─────────┘(via startWork)
    │                      │  └──[wakeUp]◄── WAITING ◄──[onRun→WAITING]
    │                      │                    │
    │                      │                    └──[shutdown]──► ABORTING
    │                      │                                      │  ▲
    │                      └──[shutdown]───────────────────────────┘  │
    │                      │                                      │  │
    │                      └──[onRun→FAILURE, retries left]──► RETRYING──►WAITING
    │                      │                                         
    │                      └──[onRun→FAILURE, no retries]──► FAILURE
    │                                                           │
    └───────────────────────────────────────────────────────────┘(via startWork)
    └◄──────────────────── ABORTED ◄──[onAbort→true]── ABORTING
```

### Lifecycle Flow

1. **Creation:** Work is constructed in PENDING state.
2. **Starting:** `startWork(callback)` transitions PENDING→RUNNING, resets retries, calls `onReset()`.
3. **Cranking:** The scheduler repeatedly calls `crankWork()`, which calls `onRun()`. The return value drives the next state transition.
4. **Waiting:** If `onRun()` returns WAITING, the work is not cranked until `wakeUp()` is called (by a timer, child notification, or external event).
5. **Retrying:** If `onRun()` returns FAILURE and retries remain, `setState` converts it to RETRYING, calls `onFailureRetry()`, `reset()`, and `waitForRetry()` which sets a timer with exponential backoff. On timer expiry, `wakeUp()` transitions back to RUNNING.
6. **Shutdown:** `shutdown()` transitions to ABORTING. Subsequent cranks call `onAbort()` until it returns true, then transition to ABORTED.
7. **Terminal states:** SUCCESS, FAILURE, ABORTED. A terminal work can be restarted via `startWork()`, which transitions back to PENDING then RUNNING.

### Retry Mechanism

Exponential backoff via `getRetryDelay()` which calls `exponentialBackoff(mRetries)`. The retry timer fires asynchronously; on expiry it increments `mRetries` and calls `wakeUp()`. Maximum retries configured per-work via `mMaxRetries`.

---

## Scheduling and Control Flow

### Main Scheduling Loop (`WorkScheduler`)

1. `scheduleWork<T>()` creates a child and wires a notification callback containing `scheduleOne()`.
2. `scheduleOne()` posts a closure to the main thread via `Application::postOnMainThread()`.
3. Inside the closure: loop calling `crankWork()` while state is RUNNING and `!clock.shouldYield()`.
4. If still RUNNING after the loop, call `scheduleOne()` again to reschedule.
5. `crankWork()` on WorkScheduler calls `Work::onRun()`, which round-robins through children.

### Round-Robin Child Scheduling (`Work::onRun`)

1. `yieldNextRunningChild()` scans from `mNextChild` iterator, skipping done children (removing them from the list).
2. If a RUNNING child is found, its `crankWork()` is called, and the parent returns RUNNING.
3. If no RUNNING child is found (all are WAITING or done), `doWork()` is called on the parent.
4. The iterator wraps around, ensuring fair round-robin scheduling across children.

### Notification Propagation

When a child's state changes (e.g., finishes, wakes up), it calls `mNotifyCallback`, which is typically `wakeSelfUpCallback()` of the parent. This propagates upward: child wakes parent, parent wakes grandparent, etc., until reaching the WorkScheduler, which calls `scheduleOne()` to post another crank on the IO service.

---

## Ownership Relationships

- **WorkScheduler** is owned by `Application` (via `shared_ptr`).
- **Work** owns its `mChildren` (list of `shared_ptr<BasicWork>`).
- **WorkSequence** owns its `mSequenceOfWork` (vector of `shared_ptr<BasicWork>`).
- **ConditionalWork** owns its `mConditionedWork` (`shared_ptr<BasicWork>`).
- **BatchWork** tracks active items in `mBatch` (map of `shared_ptr<BasicWork>`); actual ownership is via Work's `mChildren`.
- All notification callbacks capture `weak_ptr` to avoid preventing destruction.
- `BasicWork` inherits `enable_shared_from_this` and must always be held in a `shared_ptr`.

---

## Key Data Flows

### Work Creation and Execution

```
Application
  └── WorkScheduler (scheduleWork<T>)
        └── Work::addWork → child added to mChildren, startWork called
              └── child.startWork(wakeSelfUpCallback) → PENDING→RUNNING
                    └── scheduleOne → post crankWork to IO service
                          └── WorkScheduler.crankWork → Work.onRun
                                └── yieldNextRunningChild → child.crankWork
                                      └── child.onRun → state transition
```

### Abort/Shutdown Flow

```
Application::gracefulStop
  └── WorkScheduler::shutdown
        └── Work::shutdown → shutdownChildren → each child.shutdown()
              └── child: RUNNING/WAITING → ABORTING
                    └── crankWork → onAbort → true → ABORTED
                          └── parent detects allChildrenDone → ABORTED
```

### BatchWork Data Flow

```
BatchWork.doWork
  ├── anyChildRaiseFailure? → FAILURE
  ├── clean up successful children from mBatch
  ├── addMoreWorkIfNeeded
  │     └── while batch < MAX_CONCURRENT_SUBPROCESSES && hasNext
  │           └── yieldMoreWork → addWork(child)
  └── return aggregated status
```

### ConditionalWork Gating Pattern

```
ConditionalWork.onRun
  ├── mWorkStarted? → crank conditioned work, return its state
  └── !mWorkStarted
        ├── condition(app) == false → setupWaitingCallback(sleepDelay), return WAITING
        └── condition(app) == true  → startWork on conditioned work, set mWorkStarted, recurse
```
