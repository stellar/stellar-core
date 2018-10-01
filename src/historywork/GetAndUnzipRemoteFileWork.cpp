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
    Application& app, std::function<void()> callback, FileTransferInfo ft,
    std::shared_ptr<HistoryArchive> archive, size_t maxRetries)
    : Work(app, callback,
           std::string("get-and-unzip-remote-file ") + ft.remoteName(),
           maxRetries)
    , mFt(std::move(ft))
    , mArchive(archive)
{
}

std::string
GetAndUnzipRemoteFileWork::getStatus() const
{
    if (!isDone())
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
    return BasicWork::getStatus();
}

void
GetAndUnzipRemoteFileWork::doReset()
{
    mGetRemoteFileWork.reset();
    mGunzipFileWork.reset();
}

void
GetAndUnzipRemoteFileWork::onFailureRaise()
{
    std::remove(mFt.localPath_nogz().c_str());
    std::remove(mFt.localPath_gz().c_str());
    std::remove(mFt.localPath_gz_tmp().c_str());
    Work::onFailureRaise();
}

void
GetAndUnzipRemoteFileWork::onFailureRetry()
{
    std::remove(mFt.localPath_nogz().c_str());
    std::remove(mFt.localPath_gz().c_str());
    std::remove(mFt.localPath_gz_tmp().c_str());
    Work::onFailureRetry();
}

BasicWork::State
GetAndUnzipRemoteFileWork::doWork()
{
    CLOG(DEBUG, "History") << "Downloading and unzipping " << mFt.remoteName();

    if (mGunzipFileWork)
    {
        // Download completed, unzipping started

        auto state = mGunzipFileWork->getState();
        if (state == WORK_SUCCESS)
        {
            assert(mGetRemoteFileWork);
            assert(mGetRemoteFileWork->getState() == WORK_SUCCESS);
            if (!fs::exists(mFt.localPath_nogz()))
            {
                CLOG(ERROR, "History")
                    << "Downloading and unzipping " << mFt.remoteName()
                    << ": .xdr not found";
                return WORK_FAILURE_RETRY;
            }
            return WORK_SUCCESS;
        }
        else if (state == WORK_FAILURE_RAISE)
        {
            return WORK_FAILURE_RETRY;
        }
        return state;
    }
    else if (mGetRemoteFileWork)
    {
        // Download started

        auto state = mGetRemoteFileWork->getState();
        if (state == WORK_SUCCESS)
        {
            if (!validateFile())
            {
                return WORK_FAILURE_RETRY;
            }

            mGunzipFileWork =
                addWork<GunzipFileWork>(mFt.localPath_gz(), false, RETRY_NEVER);
            return WORK_RUNNING;
        }
        else if (state == WORK_FAILURE_RAISE)
        {
            return WORK_FAILURE_RETRY;
        }
        return state;
    }
    else
    {
        // Download hasn't started

        mGetRemoteFileWork = addWork<GetRemoteFileWork>(
            mFt.remoteName(), mFt.localPath_gz_tmp(), nullptr, RETRY_NEVER);
        return WORK_RUNNING;
    }
}

bool
GetAndUnzipRemoteFileWork::validateFile()
{
    if (!fs::exists(mFt.localPath_gz_tmp()))
    {
        CLOG(ERROR, "History") << "Downloading and unzipping "
                               << mFt.remoteName() << ": .tmp file not found";
        return false;
    }

    CLOG(TRACE, "History") << "Downloading and unzipping " << mFt.remoteName()
                           << ": renaming .gz.tmp to .gz";
    if (fs::exists(mFt.localPath_gz()) &&
        std::remove(mFt.localPath_gz().c_str()))
    {
        CLOG(ERROR, "History") << "Downloading and unzipping "
                               << mFt.remoteName() << ": failed to remove .gz";
        return false;
    }

    if (std::rename(mFt.localPath_gz_tmp().c_str(), mFt.localPath_gz().c_str()))
    {
        CLOG(ERROR, "History")
            << "Downloading and unzipping " << mFt.remoteName()
            << ": failed to rename .gz.tmp to .gz";
        return false;
    }

    CLOG(TRACE, "History") << "Downloading and unzipping " << mFt.remoteName()
                           << ": renamed .gz.tmp to .gz";

    if (!fs::exists(mFt.localPath_gz()))
    {
        CLOG(ERROR, "History") << "Downloading and unzipping "
                               << mFt.remoteName() << ": .gz not found";
        return false;
    }

    return true;
}
}
