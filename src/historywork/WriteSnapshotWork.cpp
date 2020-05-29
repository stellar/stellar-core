// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/WriteSnapshotWork.h"
#include "database/Database.h"
#include "history/StateSnapshot.h"
#include "historywork/Progress.h"
#include "main/Application.h"
#include "util/XDRStream.h"
#include <Tracy.hpp>

namespace stellar
{

// Note, WriteSnapshotWork does not have any special retry clean-up logic:
// history items are written via XDROutputFileStream, which automatically
// truncates any existing files.
WriteSnapshotWork::WriteSnapshotWork(Application& app,
                                     std::shared_ptr<StateSnapshot> snapshot)
    : BasicWork(app, "write-snapshot", BasicWork::RETRY_A_LOT)
    , mSnapshot(snapshot)
{
}

BasicWork::State
WriteSnapshotWork::onRun()
{
    if (mDone)
    {
        return mSuccess ? State::WORK_SUCCESS : State::WORK_FAILURE;
    }

    std::weak_ptr<WriteSnapshotWork> weak(
        std::static_pointer_cast<WriteSnapshotWork>(shared_from_this()));

    auto work = [weak]() {
        auto self = weak.lock();
        if (!self)
        {
            return;
        }
        ZoneScoped;

        auto snap = self->mSnapshot;
        bool success = true;
        if (!snap->writeHistoryBlocks())
        {
            success = false;
        }

        // Not ideal, but needed to prevent race conditions with
        // main thread, since BasicWork's state is not thread-safe. This is a
        // temporary workaround, as a cleaner solution is needed.
        self->mApp.postOnMainThread(
            [weak, success]() {
                auto self = weak.lock();
                if (self)
                {
                    self->mDone = true;
                    self->mSuccess = success;
                    self->wakeUp();
                }
            },
            "WriteSnapshotWork: finish");
    };

    // Throw the work over to a worker thread if we can use DB pools,
    // otherwise run on main thread.
    // NB: we post in both cases as to share the logic
    if (mApp.getDatabase().canUsePool())
    {
        mApp.postOnBackgroundThread(work, "WriteSnapshotWork: bgstart");
    }
    else
    {
        mApp.postOnMainThread(work, "WriteSnapshotWork: start");
    }
    return State::WORK_WAITING;
}
}
