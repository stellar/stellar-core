// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/PutRemoteFileWork.h"
#include "history/HistoryArchive.h"
#include "main/Application.h"

namespace stellar
{

PutRemoteFileWork::PutRemoteFileWork(Application& app,
                                     std::function<void()> callback,
                                     std::string const& local,
                                     std::string const& remote,
                                     std::shared_ptr<HistoryArchive> archive)
    : RunCommandWork(app, callback, std::string("put-remote-file ") + remote)
    , mRemote(remote)
    , mLocal(local)
    , mArchive(archive)
{
    assert(mArchive);
    assert(mArchive->hasPutCmd());
}

PutRemoteFileWork::~PutRemoteFileWork()
{
}

RunCommandInfo
PutRemoteFileWork::getCommand()
{
    auto cmdLine = mArchive->putFileCmd(mLocal, mRemote);
    return RunCommandInfo(cmdLine, std::string());
}

void
PutRemoteFileWork::onSuccess()
{
    mArchive->markSuccess();
}

void
PutRemoteFileWork::onFailureRaise()
{
    mArchive->markFailure();
    RunCommandWork::onFailureRaise();
}
}
