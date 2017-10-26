// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/MakeRemoteDirWork.h"
#include "history/HistoryArchive.h"

namespace stellar
{

MakeRemoteDirWork::MakeRemoteDirWork(
    Application& app, WorkParent& parent, std::string const& dir,
    std::shared_ptr<HistoryArchive const> archive)
    : RunCommandWork(app, parent, std::string("make-remote-dir ") + dir)
    , mDir(dir)
    , mArchive(archive)
{
    assert(mArchive);
}

MakeRemoteDirWork::~MakeRemoteDirWork()
{
    clearChildren();
}

void
MakeRemoteDirWork::getCommand(std::string& cmdLine, std::string& outFile)
{
    if (mArchive->hasMkdirCmd())
    {
        cmdLine = mArchive->mkdirCmd(mDir);
    }
}
}
