// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/GetRemoteFileWork.h"
#include "history/HistoryArchive.h"
#include "history/HistoryManager.h"
#include "main/Application.h"

namespace stellar
{

GetRemoteFileWork::GetRemoteFileWork(
    Application& app, WorkParent& parent, std::string const& remote,
    std::string const& local, std::shared_ptr<HistoryArchive const> archive,
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
    auto archive = mArchive;
    if (!archive)
    {
        archive = mApp.getHistoryManager().selectRandomReadableHistoryArchive();
    }
    assert(archive);
    assert(archive->hasGetCmd());
    cmdLine = archive->getFileCmd(mRemote, mLocal);
}

void
GetRemoteFileWork::onReset()
{
    std::remove(mLocal.c_str());
}
}
