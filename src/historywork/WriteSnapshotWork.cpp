// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/WriteSnapshotWork.h"
#include "database/Database.h"
#include "history/StateSnapshot.h"
#include "historywork/Progress.h"
#include "ledger/LedgerHeaderFrame.h"
#include "main/Application.h"
#include "util/XDRStream.h"

namespace stellar
{

WriteSnapshotWork::WriteSnapshotWork(Application& app,
                                     std::function<void()> callback,
                                     std::shared_ptr<StateSnapshot> snapshot)
    : BasicWork(app, callback, "write-snapshot", Work::RETRY_A_LOT)
    , mSnapshot(snapshot)
{
}

BasicWork::State
WriteSnapshotWork::onRun()
{
    if (mDone)
    {
        return mSuccess ? WORK_SUCCESS : WORK_FAILURE_RETRY;
    }

    std::weak_ptr<NewWriteSnapshotWork> weak(
        std::static_pointer_cast<WriteSnapshotWork>(shared_from_this()));

    auto handler = [weak](bool success) {
        return [weak, success]() {
            auto self = weak.lock();
            if (self)
            {
                self->mDone = true;
                self->mSuccess = success;
                self->wakeUp();
            }
        };
    };

    auto work = [weak, handler]() {
        auto self = weak.lock();
        if (!self)
        {
            return;
        }

        auto snap = self->mSnapshot;
        bool success = true;
        if (!snap->writeHistoryBlocks())
        {
            success = false;
        }

        // TODO (mlo) Not ideal, but needed to prevent race conditions with
        // main thread, since BasicWork's state is not thread-safe. This is a
        // temporary workaround, as a cleaner solution is needed.
        self->mApp.getClock().getIOService().post(handler(success));
    };

    // Throw the work over to a worker thread if we can use DB pools,
    // otherwise run on main thread.
    // TODO check if this is actually correct
    if (mApp.getDatabase().canUsePool())
    {
        mApp.getWorkerIOService().post(work);
        return WORK_WAITING;
    }
    else
    {
        work();
        return WORK_RUNNING;
    }
}
}
