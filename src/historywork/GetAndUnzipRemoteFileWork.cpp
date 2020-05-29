// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/GetAndUnzipRemoteFileWork.h"
#include "history/FileTransferInfo.h"
#include "historywork/GetRemoteFileWork.h"
#include "historywork/GunzipFileWork.h"
#include "util/Logging.h"
#include <Tracy.hpp>

namespace stellar
{

GetAndUnzipRemoteFileWork::GetAndUnzipRemoteFileWork(
    Application& app, FileTransferInfo ft,
    std::shared_ptr<HistoryArchive> archive)
    : Work(app, std::string("get-and-unzip-remote-file ") + ft.remoteName(),
           BasicWork::RETRY_A_LOT)
    , mFt(std::move(ft))
    , mArchive(archive)
    , mDownloadStart(app.getMetrics().NewMeter(
          {"history", "download-" + mFt.getType(), "start"}, "event"))
    , mDownloadSuccess(app.getMetrics().NewMeter(
          {"history", "download-" + mFt.getType(), "success"}, "event"))
    , mDownloadFailure(app.getMetrics().NewMeter(
          {"history", "download-" + mFt.getType(), "failure"}, "event"))
{
}

std::string
GetAndUnzipRemoteFileWork::getStatus() const
{
    if (mGunzipFileWork)
    {
        return mGunzipFileWork->getStatus();
    }
    else if (mGetRemoteFileWork)
    {
        return mGetRemoteFileWork->getStatus();
    }
    return BasicWork::getStatus();
}

void
GetAndUnzipRemoteFileWork::doReset()
{
    std::remove(mFt.localPath_nogz().c_str());
    std::remove(mFt.localPath_gz().c_str());
    std::remove(mFt.localPath_gz_tmp().c_str());
    mGetRemoteFileWork.reset();
    mGunzipFileWork.reset();
}

void
GetAndUnzipRemoteFileWork::onFailureRaise()
{

    mDownloadFailure.Mark();
    Work::onFailureRaise();
}

void
GetAndUnzipRemoteFileWork::onSuccess()
{
    mDownloadSuccess.Mark();
    Work::onSuccess();
}

BasicWork::State
GetAndUnzipRemoteFileWork::doWork()
{
    ZoneScoped;
    if (mGunzipFileWork)
    {
        // Download completed, unzipping started
        assert(mGetRemoteFileWork);
        assert(mGetRemoteFileWork->getState() == State::WORK_SUCCESS);
        auto state = mGunzipFileWork->getState();
        if (state == State::WORK_SUCCESS && !fs::exists(mFt.localPath_nogz()))
        {
            CLOG(ERROR, "History") << "Downloading and unzipping "
                                   << mFt.remoteName() << ": .xdr not found";
            return State::WORK_FAILURE;
        }
        return state;
    }
    else if (mGetRemoteFileWork)
    {
        // Download started
        auto state = mGetRemoteFileWork->getState();
        if (state == State::WORK_SUCCESS)
        {
            if (!validateFile())
            {
                return State::WORK_FAILURE;
            }
            mGunzipFileWork = addWork<GunzipFileWork>(mFt.localPath_gz(), false,
                                                      BasicWork::RETRY_NEVER);
            return State::WORK_RUNNING;
        }
        return state;
    }
    else
    {
        CLOG(DEBUG, "History")
            << "Downloading and unzipping " << mFt.remoteName();
        mGetRemoteFileWork =
            addWork<GetRemoteFileWork>(mFt.remoteName(), mFt.localPath_gz_tmp(),
                                       mArchive, BasicWork::RETRY_NEVER);
        mDownloadStart.Mark();
        return State::WORK_RUNNING;
    }
}

bool
GetAndUnzipRemoteFileWork::validateFile()
{
    ZoneScoped;
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
