// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/GetRemoteFileWork.h"
#include "fmt/format.h"
#include "history/HistoryArchive.h"
#include "history/HistoryArchiveManager.h"
#include "history/HistoryManager.h"
#include "main/Application.h"
#include "util/Logging.h"

namespace stellar
{
GetRemoteFileWork::GetRemoteFileWork(Application& app,
                                     std::string const& remote,
                                     std::string const& local,
                                     std::shared_ptr<HistoryArchive> archive,
                                     size_t maxRetries)
    : RunCommandWork(app, std::string("get-remote-file ") + remote, maxRetries)
    , mRemote(remote)
    , mLocal(local)
    , mArchive(archive)
{
}

CommandInfo
GetRemoteFileWork::getCommand()
{
    mCurrentArchive = mArchive;
    if (!mCurrentArchive)
    {
        mCurrentArchive = mApp.getHistoryArchiveManager()
                              .selectRandomReadableHistoryArchive();
    }
    assert(mCurrentArchive);
    assert(mCurrentArchive->hasGetCmd());
    auto cmdLine = mCurrentArchive->getFileCmd(mRemote, mLocal);

    return CommandInfo{cmdLine, std::string()};
}

void
GetRemoteFileWork::onReset()
{
    std::remove(mLocal.c_str());
    RunCommandWork::onReset();
}

void
GetRemoteFileWork::onSuccess()
{
    assert(mCurrentArchive);
    mCurrentArchive->markSuccess();
    RunCommandWork::onSuccess();
}

void
GetRemoteFileWork::onFailureRaise()
{
    assert(mCurrentArchive);
    CLOG_ERROR(History,
               "Could not download file: archive {} maybe missing file {}",
               mCurrentArchive->getName(), mRemote);
    mCurrentArchive->markFailure();
    RunCommandWork::onFailureRaise();
}

std::shared_ptr<HistoryArchive>
GetRemoteFileWork::getCurrentArchive() const
{
    return mCurrentArchive;
}
}
