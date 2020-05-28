// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/ResolveSnapshotWork.h"
#include "history/StateSnapshot.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include <Tracy.hpp>

namespace stellar
{

ResolveSnapshotWork::ResolveSnapshotWork(
    Application& app, std::shared_ptr<StateSnapshot> snapshot)
    : BasicWork(app, "prepare-snapshot", BasicWork::RETRY_NEVER)
    , mSnapshot(snapshot)
    , mTimer(std::make_unique<VirtualTimer>(app.getClock()))
{
    if (!mSnapshot)
    {
        throw std::runtime_error("ResolveSnapshotWork: invalid snapshot");
    }
}

BasicWork::State
ResolveSnapshotWork::onRun()
{
    ZoneScoped;
    if (mEc)
    {
        return State::WORK_FAILURE;
    }

    mSnapshot->mLocalState.prepareForPublish(mApp);
    mSnapshot->mLocalState.resolveAnyReadyFutures();
    if ((mApp.getLedgerManager().getLastClosedLedgerNum() >
         mSnapshot->mLocalState.currentLedger) &&
        mSnapshot->mLocalState.futuresAllResolved())
    {
        assert(mSnapshot->mLocalState.containsValidBuckets(mApp));
        return State::WORK_SUCCESS;
    }
    else
    {
        std::weak_ptr<ResolveSnapshotWork> weak(
            std::static_pointer_cast<ResolveSnapshotWork>(shared_from_this()));
        auto handler = [weak](asio::error_code const& ec) {
            auto self = weak.lock();
            if (self)
            {
                self->mEc = ec;
                self->wakeUp();
            }
        };
        mTimer->expires_from_now(std::chrono::seconds(1));
        mTimer->async_wait(handler);
        return State::WORK_WAITING;
    }
}
}
