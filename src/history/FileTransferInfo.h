// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "bucket/BucketUtils.h"
#include "crypto/Hex.h"
#include <filesystem>
#include <string>

namespace stellar
{

class Config;
class TmpDir;

inline std::string const HISTORY_LOCAL_DIR_NAME = "history";
enum class FileType
{
    HISTORY_FILE_TYPE_BUCKET,
    HISTORY_FILE_TYPE_LEDGER,
    HISTORY_FILE_TYPE_TRANSACTIONS,
    HISTORY_FILE_TYPE_RESULTS,
    HISTORY_FILE_TYPE_SCP
};

std::string typeString(FileType type);
void createPath(std::filesystem::path path);
std::filesystem::path createPublishDir(FileType type, Config const& cfg);
std::filesystem::path getPublishHistoryDir(FileType type, Config const& cfg);

class FileTransferInfo
{
    FileType mType;
    std::string mHexDigits;
    std::string mLocalPath;
    std::string getLocalDir(TmpDir const& localRoot) const;

  public:
    template <typename BucketT>
    FileTransferInfo(BucketT const& bucket)
        : mType(FileType::HISTORY_FILE_TYPE_BUCKET)
        , mHexDigits(binToHex(bucket.getHash()))
        , mLocalPath(bucket.getFilename().string())
    {
        BUCKET_TYPE_ASSERT(BucketT);
    }

    FileTransferInfo(TmpDir const& snapDir, FileType const& snapType,
                     uint32_t checkpointLedger);

    FileTransferInfo(FileType const& snapType, uint32_t checkpointLedger,
                     Config const& cfg);

    FileTransferInfo(TmpDir const& snapDir, FileType const& snapType,
                     std::string const& hexDigits);

    FileType
    getType() const
    {
        return mType;
    }

    std::string
    getTypeString() const
    {
        return typeString(mType);
    }

    std::string
    localPath_nogz() const
    {
        return mLocalPath;
    }

    std::string
    localPath_nogz_dirty() const
    {
        return mLocalPath + ".dirty";
    }

    std::string
    localPath_gz() const
    {
        return mLocalPath + ".gz";
    }
    std::string
    localPath_gz_tmp() const
    {
        return mLocalPath + ".gz.tmp";
    }

    std::string baseName_nogz() const;
    std::string baseName_gz() const;
    std::string baseName_gz_tmp() const;
    std::string remoteDir() const;
    std::string remoteName() const;
};
}
