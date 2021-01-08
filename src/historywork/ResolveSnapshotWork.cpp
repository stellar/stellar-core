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
        setupWaitingCallback(std::chrono::seconds(1));
        return State::WORK_WAITING;
    }
}
}
