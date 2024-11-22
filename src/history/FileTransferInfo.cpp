// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "FileTransferInfo.h"
#include <Tracy.hpp>
#include <thread>

namespace stellar
{

void
createPath(std::filesystem::path path)
{
    if (fs::exists(path.string()))
    {
        return;
    }

    int retries = 5;
    // Similarly to TmpDir, retry in case there were
    // OS-related errors (e.g. out of memory) or race conditions
    while (!fs::mkpath(path.string()))
    {
        if (--retries == 0)
        {
            throw std::runtime_error("Unable to make a path " + path.string());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

std::string
FileTransferInfo::getLocalDir(TmpDir const& localRoot) const
{
    ZoneScoped;
    auto localDir = localRoot.getName();
    localDir += "/" + fs::remoteDir(typeString(mType), mHexDigits);
    createPath(localDir);
    return localDir;
}

std::string
typeString(FileType type)
{
    switch (type)
    {
    case FileType::HISTORY_FILE_TYPE_BUCKET:
        return "bucket";
    case FileType::HISTORY_FILE_TYPE_LEDGER:
        return "ledger";
    case FileType::HISTORY_FILE_TYPE_TRANSACTIONS:
        return "transactions";
    case FileType::HISTORY_FILE_TYPE_RESULTS:
        return "results";
    case FileType::HISTORY_FILE_TYPE_SCP:
        return "scp";
    }
}

std::filesystem::path
createPublishDir(FileType type, Config const& cfg)
{
    std::filesystem::path root = cfg.BUCKET_DIR_PATH;
    auto path = getPublishHistoryDir(type, cfg);
    createPath(path);
    return path;
}

std::filesystem::path
getPublishHistoryDir(FileType type, Config const& cfg)
{
    std::filesystem::path root = cfg.BUCKET_DIR_PATH;
    return root / HISTORY_LOCAL_DIR_NAME / typeString(type);
}
}
