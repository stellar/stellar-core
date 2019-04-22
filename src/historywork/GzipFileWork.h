// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "historywork/RunCommandWork.h"

namespace stellar
{

class GzipFileWork : public RunCommandWork
{
    std::string const mFilenameNoGz;
    bool const mKeepExisting;
    CommandInfo getCommand() override;

  public:
    GzipFileWork(Application& app, std::string const& filenameNoGz,
                 bool keepExisting = false);
    ~GzipFileWork() = default;

  protected:
    void onReset() override;
};
}
