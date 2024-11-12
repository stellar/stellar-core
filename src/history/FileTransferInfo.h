#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/LiveBucket.h"
#include "crypto/Hex.h"
#include "main/Config.h"
#include "util/Fs.h"
#include "util/TmpDir.h"
#include <string>

namespace stellar
{

std::string const HISTORY_LOCAL_DIR_NAME = "history";
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
    FileTransferInfo(LiveBucket const& bucket)
        : mType(FileType::HISTORY_FILE_TYPE_BUCKET)
        , mHexDigits(binToHex(bucket.getHash()))
        , mLocalPath(bucket.getFilename().string())
    {
    }

    FileTransferInfo(TmpDir const& snapDir, FileType const& snapType,
                     uint32_t checkpointLedger)
        : mType(snapType)
        , mHexDigits(fs::hexStr(checkpointLedger))
        , mLocalPath(getLocalDir(snapDir) + "/" + baseName_nogz())
    {
    }

    FileTransferInfo(FileType const& snapType, uint32_t checkpointLedger,
                     Config const& cfg)
        : mType(snapType)
        , mHexDigits(fs::hexStr(checkpointLedger))
        , mLocalPath(getPublishHistoryDir(snapType, cfg).string() + "/" +
                     baseName_nogz())
    {
    }

    FileTransferInfo(TmpDir const& snapDir, FileType const& snapType,
                     std::string const& hexDigits)
        : mType(snapType)
        , mHexDigits(hexDigits)
        , mLocalPath(getLocalDir(snapDir) + "/" + baseName_nogz())
    {
    }

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

    std::string
    baseName_nogz() const
    {
        return fs::baseName(getTypeString(), mHexDigits, "xdr");
    }
    std::string
    baseName_gz() const
    {
        return baseName_nogz() + ".gz";
    }
    std::string
    baseName_gz_tmp() const
    {
        return baseName_nogz() + ".gz.tmp";
    }

    std::string
    remoteDir() const
    {
        return fs::remoteDir(getTypeString(), mHexDigits);
    }
    std::string
    remoteName() const
    {
        return fs::remoteName(getTypeString(), mHexDigits, "xdr.gz");
    }
};
}
