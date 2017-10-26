// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/PublishWork.h"
#include "history/HistoryManager.h"
#include "history/StateSnapshot.h"
#include "historywork/PutSnapshotFilesWork.h"
#include "historywork/ResolveSnapshotWork.h"
#include "historywork/WriteSnapshotWork.h"
#include "lib/util/format.h"
#include "main/Application.h"
#include "util/Logging.h"

namespace stellar
{

PublishWork::PublishWork(Application& app, WorkParent& parent,
                         std::shared_ptr<StateSnapshot> snapshot)
    : Work(app, parent,
           fmt::format("publish-{:08x}", snapshot->mLocalState.currentLedger))
    , mSnapshot(snapshot)
    , mOriginalBuckets(mSnapshot->mLocalState.allBuckets())
{
}

PublishWork::~PublishWork()
{
    clearChildren();
}

std::string
PublishWork::getStatus() const
{
    if (mState == WORK_PENDING)
    {
        if (mResolveSnapshotWork)
        {
            return mResolveSnapshotWork->getStatus();
        }
        else if (mWriteSnapshotWork)
        {
            return mWriteSnapshotWork->getStatus();
        }
        else if (mUpdateArchivesWork)
        {
            return mUpdateArchivesWork->getStatus();
        }
    }
    return Work::getStatus();
}

void
PublishWork::onReset()
{
    clearChildren();

    mResolveSnapshotWork.reset();
    mWriteSnapshotWork.reset();
    mUpdateArchivesWork.reset();
}

Work::State
PublishWork::onSuccess()
{
    // Phase 1: resolve futures in snapshot
    if (!mResolveSnapshotWork)
    {
        mResolveSnapshotWork = addWork<ResolveSnapshotWork>(mSnapshot);
        return WORK_PENDING;
    }

    // Phase 2: write snapshot files
    if (!mWriteSnapshotWork)
    {
        mWriteSnapshotWork = addWork<WriteSnapshotWork>(mSnapshot);
        return WORK_PENDING;
    }

    // Phase 3: update archives
    if (!mUpdateArchivesWork)
    {
        mUpdateArchivesWork = addWork<Work>("update-archives");
        for (auto& aPair : mApp.getConfig().HISTORY)
        {
            auto arch = aPair.second;
            if (!arch->hasPutCmd())
            {
                continue;
            }
            mUpdateArchivesWork->addWork<PutSnapshotFilesWork>(arch, mSnapshot);
        }
        return WORK_PENDING;
    }

    // use mOriginalBuckets as mSnapshot->mLocalState.allBuckets() could change
    // in meantime
    mApp.getHistoryManager().historyPublished(
        mSnapshot->mLocalState.currentLedger, mOriginalBuckets, true);
    return WORK_SUCCESS;
}

void
PublishWork::onFailureRaise()
{
    // use mOriginalBuckets as mSnapshot->mLocalState.allBuckets() could change
    // in meantime
    mApp.getHistoryManager().historyPublished(
        mSnapshot->mLocalState.currentLedger, mOriginalBuckets, false);
}
}
