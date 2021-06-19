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

    static std::filesystem::path
    getMetaDebugFilePath(std::filesystem::path const& bucketDir,
                         uint32_t seqNum);

    static std::filesystem::path
    getMetaDebugDirPath(std::filesystem::path const& bucketDir);

    static std::vector<std::filesystem::path>
    listMetaDebugFiles(std::filesystem::path const& bucketDir);

    static bool isDebugSegmentBoundary(uint32_t ledgerSeq);

    static size_t getNumberOfDebugFilesToKeep(uint32_t ledgersToKeep);

  protected:
    BasicWork::State doWork() override;
};
}
