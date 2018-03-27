// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/GetRemoteFileWork.h"
#include "history/HistoryArchive.h"
#include "history/HistoryArchiveManager.h"
#include "history/HistoryManager.h"
#include "main/Application.h"

namespace stellar
{

GetRemoteFileWork::GetRemoteFileWork(Application& app, WorkParent& parent,
                                     std::string const& remote,
                                     std::string const& local,
                                     std::shared_ptr<HistoryArchive> archive,
                                     size_t maxRetries)
    : RunCommandWork(app, parent, std::string("get-remote-file ") + remote,
                     maxRetries)
    , mRemote(remote)
    , mLocal(local)
    , mArchive(archive)
{
}

GetRemoteFileWork::~GetRemoteFileWork()
{
    clearChildren();
}

void
GetRemoteFileWork::getCommand(std::string& cmdLine, std::string& outFile)
{
    mCurrentArchive = mArchive;
    if (!mCurrentArchive)
    {
        mCurrentArchive = mApp.getHistoryArchiveManager()
                              .selectRandomReadableHistoryArchive();
    }
    assert(mCurrentArchive);
    assert(mCurrentArchive->hasGetCmd());
    cmdLine = mCurrentArchive->getFileCmd(mRemote, mLocal);
}

void
GetRemoteFileWork::onReset()
{
    std::remove(mLocal.c_str());
}

Work::State
GetRemoteFileWork::onSuccess()
{
    assert(mCurrentArchive);
    mCurrentArchive->markSuccess();
    return RunCommandWork::onSuccess();
}

void
GetRemoteFileWork::onFailureRaise()
{
    assert(mCurrentArchive);
    mCurrentArchive->markFailure();
    RunCommandWork::onFailureRaise();
}
}
