// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "historywork/RunCommandWork.h"

namespace stellar
{

class GunzipFileWork : public RunCommandWork
{
    std::string const mFilenameGz;
    bool const mKeepExisting;
    CommandInfo getCommand() override;

  public:
    GunzipFileWork(Application& app, std::string const& filenameGz,
                   bool keepExisting = false,
                   size_t maxRetries = Work::RETRY_NEVER);
    ~GunzipFileWork() = default;

  protected:
    void onReset() override;
};
}
