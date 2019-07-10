// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/MakeRemoteDirWork.h"
#include "history/HistoryArchive.h"
#include "main/Application.h"

namespace stellar
{

MakeRemoteDirWork::MakeRemoteDirWork(Application& app, std::string const& dir,
                                     std::shared_ptr<HistoryArchive> archive)
    : RunCommandWork(app, std::string("make-remote-dir ") + dir,
                     BasicWork::RETRY_A_LOT)
    , mDir(dir)
    , mArchive(archive)
{
    assert(mArchive);
}

CommandInfo
MakeRemoteDirWork::getCommand()
{
    std::string cmdLine;
    if (mArchive->hasMkdirCmd())
    {
        cmdLine = mArchive->mkdirCmd(mDir);
    }
    return CommandInfo{cmdLine, std::string()};
}

void
MakeRemoteDirWork::onSuccess()
{
    mArchive->markSuccess();
}

void
MakeRemoteDirWork::onFailureRaise()
{
    mArchive->markFailure();
    RunCommandWork::onFailureRaise();
}
}
