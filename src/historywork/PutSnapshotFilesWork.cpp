// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/PutSnapshotFilesWork.h"
#include "bucket/BucketManager.h"
#include "history/HistoryArchiveManager.h"
#include "history/StateSnapshot.h"
#include "historywork/GetHistoryArchiveStateWork.h"
#include "historywork/GzipFileWork.h"
#include "historywork/PutFilesWork.h"
#include "historywork/PutHistoryArchiveStateWork.h"
#include "main/Application.h"
#include "work/WorkSequence.h"
#include <Tracy.hpp>
#include <fmt/format.h>

namespace stellar
{

PutSnapshotFilesWork::PutSnapshotFilesWork(
    Application& app, std::shared_ptr<StateSnapshot> snapshot)
    : Work(app,
           fmt::format("update-archives-{:08x}",
                       snapshot->mLocalState.currentLedger),
           // Each put-snapshot-sequence will retry correctly
           BasicWork::RETRY_NEVER)
    , mSnapshot(snapshot)
{
}

BasicWork::State
PutSnapshotFilesWork::doWork()
{
    ZoneScoped;
    if (!mUploadSeqs.empty())
    {
        return WorkUtils::getWorkStatus(mUploadSeqs);
    }

    if (!mGzipFilesWorks.empty())
    {
        if (WorkUtils::getWorkStatus(mGzipFilesWorks) == State::WORK_SUCCESS)
        {
            // Step 3: ready to upload files to archives
            for (auto const& getState : mGetStateWorks)
            {
                auto putSnapshotFiles = std::make_shared<PutFilesWork>(
                    mApp, getState->getArchive(), mSnapshot,
                    getState->getHistoryArchiveState());
                auto putArchiveState =
                    std::make_shared<PutHistoryArchiveStateWork>(
                        mApp, mSnapshot->mLocalState, getState->getArchive());

                std::vector<std::shared_ptr<BasicWork>> seq{putSnapshotFiles,
                                                            putArchiveState};
                mUploadSeqs.emplace_back(addWork<WorkSequence>(
                    "upload-files-seq", seq, BasicWork::RETRY_NEVER));
            }
            return State::WORK_RUNNING;
        }
        else
        {
            return WorkUtils::getWorkStatus(mGzipFilesWorks);
        }
    }

    if (!mGetStateWorks.empty())
    {
        std::list<std::shared_ptr<BasicWork>> works(mGetStateWorks.begin(),
                                                    mGetStateWorks.end());
        auto status = WorkUtils::getWorkStatus(works);
        if (status == State::WORK_SUCCESS)
        {
            // Step 2: Gzip all unique files
            for (auto const& f : getFilesToZip())
            {
                mGzipFilesWorks.emplace_back(addWork<GzipFileWork>(f, true));
            }
            return State::WORK_RUNNING;
        }
        else
        {
            return status;
        }
    }

    // Step 1: Get all archive states
    for (auto const& archive :
         mApp.getHistoryArchiveManager().getWritableHistoryArchives())
    {
        mGetStateWorks.emplace_back(addWork<GetHistoryArchiveStateWork>(
            0, archive, "publish", BasicWork::RETRY_A_FEW));
    }

    return State::WORK_RUNNING;
}

void
PutSnapshotFilesWork::doReset()
{
    mGetStateWorks.clear();
    mGzipFilesWorks.clear();
    mUploadSeqs.clear();
}

UnorderedSet<std::string>
PutSnapshotFilesWork::getFilesToZip()
{
    // Sanity check: there are states for all archives
    if (mGetStateWorks.size() !=
        mApp.getHistoryArchiveManager().getWritableHistoryArchives().size())
    {
        throw std::runtime_error("Corrupted GetHistoryArchiveStateWork");
    }

    UnorderedSet<std::string> filesToZip{};
    for (auto const& getState : mGetStateWorks)
    {
        for (auto const& f :
             mSnapshot->differingHASFiles(getState->getHistoryArchiveState()))
        {
            filesToZip.insert(f->localPath_nogz());
        }
    }

    return filesToZip;
}

std::string
PutSnapshotFilesWork::getStatus() const
{
    if (!mUploadSeqs.empty())
    {
        return fmt::format("{}:uploading files", getName());
    }

    if (!mGzipFilesWorks.empty())
    {
        return fmt::format("{}:zipping files", getName());
    }

    if (!mGetStateWorks.empty())
    {
        return fmt::format("{}:getting archives", getName());
    }

    return BasicWork::getStatus();
}
}
