// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "historywork/RunCommandWork.h"

namespace stellar
{

class HistoryArchive;

class MakeRemoteDirWork : public RunCommandWork
{
    std::string mDir;
    std::shared_ptr<HistoryArchive> mArchive;
    void getCommand(std::string& cmdLine, std::string& outFile) override;

  public:
    MakeRemoteDirWork(Application& app, WorkParent& parent,
                      std::string const& dir,
                      std::shared_ptr<HistoryArchive> archive);
    ~MakeRemoteDirWork();

    Work::State onSuccess() override;
    void onFailureRaise() override;
};
}
