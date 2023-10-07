// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "historywork/GzipFileWork.h"
#include "util/XDRStream.h"
#include <filesystem>

namespace stellar
{

class FlushAndRotateMetaDebugWork : public Work
{
    std::filesystem::path mMetaDebugPath;
    std::unique_ptr<XDROutputFileStream> mMetaDebugFile;
    std::shared_ptr<GzipFileWork> mGzipFileWork;
    uint32_t mLedgersToKeep;

  public:
    FlushAndRotateMetaDebugWork(
        Application& app, std::filesystem::path const& metaDebugPath,
        std::unique_ptr<XDROutputFileStream> metaDebugFile,
        uint32_t ledgersToKeep);
    ~FlushAndRotateMetaDebugWork() = default;

  protected:
    BasicWork::State doWork() override;
};
}
