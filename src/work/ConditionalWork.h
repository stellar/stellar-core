// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0
#pragma once

#include "work/BasicWork.h"

namespace stellar
{

// Any function used as a ConditionFn should be monotonic: return false for the
// first 0 or more times it's called, then if ever returns true, always return
// true thereafter. It will be called repeatedly until it returns true, but
// never again (it will actually be deleted once it returns true
// once). ConditionalWork makes its own lifecycle transitions and may call
// ConditionFn repeatedly; ConditionFn should not make any assumptions about the
// number of times or order in which it's called.
using ConditionFn = std::function<bool()>;

// A `ConditionalWork` is a work _gated_ on some arbitrary (and monotonic: see
// above) `ConditionFn`. It will remain in `WORK_WAITING` state polling the
// `ConditionFn` and rescheduling itself at `sleepTime` frequency until the
// `ConditionFn` is true, after which it will delegate to the `conditionedWork`.
//
// This exists to enable adding sequential dependency edges to work that is
// otherwise principally organized into parallel work units.
//
// For example, DownloadApplyTxsWork is _principally_ a BatchWork that runs as
// many of its yielded WorkSequence units in parallel as it can; but those units
// also have a sequential dependency among their second (apply) sub-steps, so
// those are each wrapped in a ConditionalWork, conditioned on the
// previously-yielded WorkSequence. This produces a picture like so:
//
//
//                    ┌────────────────────────────────┐
//                    │DownloadApplyTxsWork : BatchWork│
//                    └────────────────────────────────┘
//                                     │
//                   ┌─────────────────┴────────────────┐
//                   │                                  │
//                   ▼                                  ▼
//     ┌───────────────────────────┐      ┌───────────────────────────┐
//     │WorkSequence               │      │WorkSequence               │
//     │                           │      │                           │
//     │┌─────────────────────────┐│      │┌─────────────────────────┐│
//     ││GetAndUnzipRemoteFileWork││      ││GetAndUnzipRemoteFileWork││
//     │└─────────────────────────┘│      │└─────────────────────────┘│
//     │┌─────────────────────────┐│◀──┐  │┌─────────────────────────┐│
//     ││ConditionalWork          ││   │  ││ConditionalWork          ││
//     ││┌───────────────────────┐││   │  ││┌───────────────────────┐││
//     │││ApplyCheckpointWork    │││   └──┼┤│ApplyCheckpointWork    │││
//     ││└───────────────────────┘││      ││└───────────────────────┘││
//     │└─────────────────────────┘│      │└─────────────────────────┘│
//     └───────────────────────────┘      └───────────────────────────┘
//

class ConditionalWork : public BasicWork
{
    ConditionFn mCondition;
    std::shared_ptr<BasicWork> mConditionedWork;
    std::chrono::milliseconds const mSleepDelay;
    bool mWorkStarted{false};

  public:
    ConditionalWork(
        Application& app, std::string name, ConditionFn condition,
        std::shared_ptr<BasicWork> conditionedWork,
        std::chrono::milliseconds sleepTime = std::chrono::milliseconds(100));
    void shutdown() override;

    std::string getStatus() const override;

  protected:
    BasicWork::State onRun() override;
    bool onAbort() override;
    void onReset() override;
};
}
