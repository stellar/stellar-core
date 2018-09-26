// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "historywork/RunCommandWork.h"

namespace stellar
{

class GunzipFileWork : public RunCommandWork
{
    std::string mFilenameGz;
    bool mKeepExisting;
    RunCommandInfo getCommand() override;

  public:
    GunzipFileWork(Application& app, std::function<void()> callback,
                   std::string const& filenameGz, bool keepExisting = false,
                   size_t maxRetries = Work::RETRY_NEVER);
    ~GunzipFileWork() = default;

  protected:
    void onFailureRaise() override;
    void onFailureRetry() override;
};
}
