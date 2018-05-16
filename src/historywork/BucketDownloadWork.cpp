// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/BucketDownloadWork.h"
#include "main/Application.h"
#include "util/TmpDir.h"

namespace stellar
{

BucketDownloadWork::BucketDownloadWork(Application& app, WorkParent& parent,
                                       std::string const& uniqueName,
                                       HistoryArchiveState const& localState,
                                       size_t maxRetries)
    : Work(app, parent, uniqueName, maxRetries)
    , mLocalState(localState)
    , mDownloadDir(std::make_unique<TmpDir>(
          mApp.getTmpDirManager().tmpDir(getUniqueName())))
{
}

BucketDownloadWork::~BucketDownloadWork()
{
    clearChildren();
}

void
BucketDownloadWork::onReset()
{
    clearChildren();
    mBuckets.clear();
}

void
BucketDownloadWork::takeDownloadDir(BucketDownloadWork& other)
{
    if (other.mDownloadDir)
    {
        mDownloadDir = std::move(other.mDownloadDir);
    }
}
}
