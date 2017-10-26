// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "historywork/RunCommandWork.h"

namespace stellar
{

class GzipFileWork : public RunCommandWork
{
    std::string mFilenameNoGz;
    bool mKeepExisting;
    void getCommand(std::string& cmdLine, std::string& outFile) override;

  public:
    GzipFileWork(Application& app, WorkParent& parent,
                 std::string const& filenameNoGz, bool keepExisting = false);
    ~GzipFileWork();
    void onReset() override;
};
}
