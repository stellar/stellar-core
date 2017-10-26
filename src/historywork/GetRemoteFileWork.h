// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "historywork/RunCommandWork.h"

namespace stellar
{

class HistoryArchive;

class GetRemoteFileWork : public RunCommandWork
{
    std::string mRemote;
    std::string mLocal;
    std::shared_ptr<HistoryArchive const> mArchive;
    void getCommand(std::string& cmdLine, std::string& outFile) override;

  public:
    // Passing `nullptr` for the archive argument will cause the work to
    // select a new readable history archive at random each time it runs /
    // retries.
    GetRemoteFileWork(Application& app, WorkParent& parent,
                      std::string const& remote, std::string const& local,
                      std::shared_ptr<HistoryArchive const> archive = nullptr,
                      size_t maxRetries = Work::RETRY_A_LOT);
    ~GetRemoteFileWork();
    void onReset() override;
};
}
