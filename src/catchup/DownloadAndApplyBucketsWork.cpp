// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/DownloadAndApplyBucketsWork.h"
#include "catchup/ApplyBucketsWork.h"
#include "history/FileTransferInfo.h"
#include "historywork/GetAndUnzipRemoteFileWork.h"
#include "historywork/VerifyBucketWork.h"
#include "ledger/LedgerManager.h"
#include "util/Logging.h"

namespace stellar
{

DownloadAndApplyBucketsWork::DownloadAndApplyBucketsWork(
    Application& app, WorkParent& parent, HistoryArchiveState remoteState,
    std::vector<std::string> bucketsHashes, LedgerHeaderHistoryEntry applyAt,
    TmpDir const& downloadDir)
    : Work(app, parent, "download-and-apply-buckets")
    , mDownloadDir{downloadDir}
    , mRemoteState{std::move(remoteState)}
    , mBucketsHashes{std::move(bucketsHashes)}
    , mApplyAt{std::move(applyAt)}
{
}

std::string
DownloadAndApplyBucketsWork::getStatus() const
{
    if (mState == WORK_PENDING)
    {
        if (mApplyWork)
        {
            return mApplyWork->getStatus();
        }
        else if (mDownloadBucketsWork)
        {
            return mDownloadBucketsWork->getStatus();
        }
    }
    return Work::getStatus();
}

void
DownloadAndApplyBucketsWork::onReset()
{
    Work::onReset();
    clearChildren();
    mDownloadBucketsWork.reset();
    mApplyWork.reset();
}

Work::State
DownloadAndApplyBucketsWork::onSuccess()
{
    if (!mDownloadBucketsWork)
    {
        CLOG(INFO, "History") << "Catchup downloading and verifying buckets";
        mDownloadBucketsWork = addWork<Work>("download and verify buckets");
        for (auto const& hash : mBucketsHashes)
        {
            FileTransferInfo ft(mDownloadDir, HISTORY_FILE_TYPE_BUCKET, hash);
            // Each bucket gets its own work-chain of download->gunzip->verify

            auto verify = mDownloadBucketsWork->addWork<VerifyBucketWork>(
                mBuckets, ft.localPath_nogz(), hexToBin256(hash));
            verify->addWork<GetAndUnzipRemoteFileWork>(ft);
        }
        return WORK_PENDING;
    }
    assert(mDownloadBucketsWork->getState() == WORK_SUCCESS);

    if (!mApplyWork)
    {
        CLOG(INFO, "History") << "Catchup applying buckets for state "
                              << LedgerManager::ledgerAbbrev(mApplyAt);
        mApplyWork =
            addWork<ApplyBucketsWork>(mBuckets, mRemoteState, mApplyAt);
        return WORK_PENDING;
    }
    assert(mApplyWork->getState() == WORK_SUCCESS);

    return WORK_SUCCESS;
}
}
