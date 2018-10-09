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
    RunCommandInfo getCommand() override;

  public:
    MakeRemoteDirWork(Application& app, std::function<void()> callback,
                      std::string const& dir,
                      std::shared_ptr<HistoryArchive> archive);
    ~MakeRemoteDirWork();

  protected:
    void onSuccess() override;
    void onFailureRaise() override;
};
}
