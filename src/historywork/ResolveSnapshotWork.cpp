// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/ResolveSnapshotWork.h"
#include "history/StateSnapshot.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"

namespace stellar
{

ResolveSnapshotWork::ResolveSnapshotWork(
    Application& app, std::function<void()> callback,
    std::shared_ptr<StateSnapshot> snapshot)
    : BasicWork(app, callback, "prepare-snapshot", Work::RETRY_NEVER)
    , mSnapshot(snapshot)
    , mTimer(std::make_unique<VirtualTimer>(app.getClock()))
{
}

BasicWork::State
ResolveSnapshotWork::onRun()
{
    mSnapshot->mLocalState.resolveAnyReadyFutures();
    mSnapshot->makeLive();
    if ((mApp.getLedgerManager().getLastClosedLedgerNum() >
         mSnapshot->mLocalState.currentLedger) &&
        mSnapshot->mLocalState.futuresAllResolved())
    {
        return WORK_SUCCESS;
    }
    else
    {
        std::weak_ptr<ResolveSnapshotWork> weak(
            std::static_pointer_cast<ResolveSnapshotWork>(shared_from_this()));
        auto handler = [weak](asio::error_code const& ec) {
            auto self = weak.lock();
            if (self)
            {
                self->wakeUp();
            }
        };
        mTimer->expires_from_now(mDelay);
        mTimer->async_wait(handler);
        return WORK_WAITING;
    }
}
}
