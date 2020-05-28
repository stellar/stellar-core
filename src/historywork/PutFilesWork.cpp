// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "PutFilesWork.h"
#include "bucket/BucketManager.h"
#include "historywork/GzipFileWork.h"
#include "historywork/MakeRemoteDirWork.h"
#include "historywork/PutRemoteFileWork.h"
#include "work/WorkSequence.h"
#include <Tracy.hpp>

namespace stellar
{

PutFilesWork::PutFilesWork(Application& app,
                           std::shared_ptr<HistoryArchive> archive,
                           std::shared_ptr<StateSnapshot> snapshot,
                           HistoryArchiveState const& remoteState)
    // Each mkdir-and-put-file sequence will retry correctly
    : Work(app, "helper-put-files-" + archive->getName(),
           BasicWork::RETRY_NEVER)
    , mArchive(archive)
    , mSnapshot(snapshot)
    , mRemoteState(remoteState)
{
}

BasicWork::State
PutFilesWork::doWork()
{
    ZoneScoped;
    if (!mChildrenSpawned)
    {
        for (auto const& f : mSnapshot->differingHASFiles(mRemoteState))
        {
            auto mkdir = std::make_shared<MakeRemoteDirWork>(
                mApp, f->remoteDir(), mArchive);
            auto putFile = std::make_shared<PutRemoteFileWork>(
                mApp, f->localPath_gz(), f->remoteName(), mArchive);

            std::vector<std::shared_ptr<BasicWork>> seq{mkdir, putFile};
            // Each inner step will retry a lot, so retry the sequence once
            // in case of an unexpected failure
            addWork<WorkSequence>("mkdir-and-put-file-" + f->localPath_gz(),
                                  seq, BasicWork::RETRY_ONCE);
        }
        mChildrenSpawned = true;
        return State::WORK_RUNNING;
    }
    else
    {
        return checkChildrenStatus();
    }
}

void
PutFilesWork::doReset()
{
    mChildrenSpawned = false;
}
}
