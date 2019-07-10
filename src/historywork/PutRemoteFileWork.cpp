// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/PutRemoteFileWork.h"
#include "history/HistoryArchive.h"
#include "main/Application.h"

namespace stellar
{
PutRemoteFileWork::PutRemoteFileWork(Application& app, std::string const& local,
                                     std::string const& remote,
                                     std::shared_ptr<HistoryArchive> archive)
    : RunCommandWork(app, std::string("put-remote-file ") + remote,
                     BasicWork::RETRY_A_LOT)
    , mLocal(local)
    , mRemote(remote)
    , mArchive(archive)
{
    assert(mArchive);
    assert(mArchive->hasPutCmd());
}

CommandInfo
PutRemoteFileWork::getCommand()
{
    auto cmdLine = mArchive->putFileCmd(mLocal, mRemote);
    return CommandInfo{cmdLine, std::string()};
}

void
PutRemoteFileWork::onSuccess()
{
    mArchive->markSuccess();
    RunCommandWork::onSuccess();
}

void
PutRemoteFileWork::onFailureRaise()
{
    mArchive->markFailure();
    RunCommandWork::onFailureRaise();
}
}
