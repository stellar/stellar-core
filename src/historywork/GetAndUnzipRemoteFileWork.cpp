// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/GetAndUnzipRemoteFileWork.h"
#include "history/FileTransferInfo.h"
#include "historywork/GetRemoteFileWork.h"
#include "historywork/GunzipFileWork.h"
#include "util/Logging.h"

namespace stellar
{

GetAndUnzipRemoteFileWork::GetAndUnzipRemoteFileWork(
    Application& app, WorkParent& parent, FileTransferInfo ft,
    std::shared_ptr<HistoryArchive const> archive, size_t maxRetries)
    : Work(app, parent,
           std::string("get-and-unzip-remote-file ") + ft.remoteName(),
           maxRetries)
    , mFt(std::move(ft))
    , mArchive(archive)
{
}

GetAndUnzipRemoteFileWork::~GetAndUnzipRemoteFileWork()
{
    clearChildren();
}

std::string
GetAndUnzipRemoteFileWork::getStatus() const
{
    if (mState == WORK_PENDING)
    {
        if (mGunzipFileWork)
        {
            return mGunzipFileWork->getStatus();
        }
        else if (mGetRemoteFileWork)
        {
            return mGetRemoteFileWork->getStatus();
        }
    }
    return Work::getStatus();
}

void
GetAndUnzipRemoteFileWork::onReset()
{
    clearChildren();
    mGetRemoteFileWork.reset();
    mGunzipFileWork.reset();

    CLOG(DEBUG, "History") << "Downloading and unzipping " << mFt.remoteName()
                           << ": downloading";
    mGetRemoteFileWork = addWork<GetRemoteFileWork>(
        mFt.remoteName(), mFt.localPath_gz_tmp(), nullptr, RETRY_NEVER);
}

Work::State
GetAndUnzipRemoteFileWork::onSuccess()
{
    if (mGunzipFileWork)
    {
        if (!fs::exists(mFt.localPath_nogz()))
        {
            CLOG(ERROR, "History") << "Downloading and unzipping "
                                   << mFt.remoteName() << ": .xdr not found";
            return WORK_FAILURE_RETRY;
        }
        else
        {
            return WORK_SUCCESS;
        }
    }

    if (fs::exists(mFt.localPath_gz_tmp()))
    {
        CLOG(TRACE, "History")
            << "Downloading and unzipping " << mFt.remoteName()
            << ": renaming .gz.tmp to .gz";
        if (fs::exists(mFt.localPath_gz()) &&
            std::remove(mFt.localPath_gz().c_str()))
        {
            CLOG(ERROR, "History")
                << "Downloading and unzipping " << mFt.remoteName()
                << ": failed to remove .gz";
            return WORK_FAILURE_RETRY;
        }

        if (std::rename(mFt.localPath_gz_tmp().c_str(),
                        mFt.localPath_gz().c_str()))
        {
            CLOG(ERROR, "History")
                << "Downloading and unzipping " << mFt.remoteName()
                << ": failed to rename .gz.tmp to .gz";
            return WORK_FAILURE_RETRY;
        }

        CLOG(TRACE, "History")
            << "Downloading and unzipping " << mFt.remoteName()
            << ": renamed .gz.tmp to .gz";
    }

    if (!fs::exists(mFt.localPath_gz()))
    {
        CLOG(ERROR, "History") << "Downloading and unzipping "
                               << mFt.remoteName() << ": .gz not found";
        return WORK_FAILURE_RETRY;
    }

    CLOG(DEBUG, "History") << "Downloading and unzipping " << mFt.remoteName()
                           << ": unzipping";
    mGunzipFileWork =
        addWork<GunzipFileWork>(mFt.localPath_gz(), false, RETRY_NEVER);
    return WORK_PENDING;
}

void
GetAndUnzipRemoteFileWork::onFailureRaise()
{
    std::remove(mFt.localPath_nogz().c_str());
    std::remove(mFt.localPath_gz().c_str());
    std::remove(mFt.localPath_gz_tmp().c_str());
}
}
