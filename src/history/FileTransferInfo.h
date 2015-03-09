#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "clf/Bucket.h"
#include "crypto/Hex.h"
#include "util/Logging.h"
#include "util/TmpDir.h"
#include "lib/util/format.h"
#include <string>
#include <regex>

namespace stellar
{

extern char const* HISTORY_FILE_TYPE_BUCKET;
extern char const* HISTORY_FILE_TYPE_LEDGER;
extern char const* HISTORY_FILE_TYPE_TRANSACTION;

template <typename T>
class
FileTransferInfo
{
    T mTransferState;
    std::string mType;
    std::string mHexDigits;
    std::string mLocalPath;

public:

    FileTransferInfo(T state, Bucket const& bucket)
        : mTransferState(state)
        , mType(HISTORY_FILE_TYPE_BUCKET)
        , mHexDigits(binToHex(bucket.getHash()))
        , mLocalPath(bucket.getFilename())
        {}

    FileTransferInfo(T state,
                     TmpDir const& snapDir,
                     std::string const& snapType,
                     uint32_t checkpointNum)
        : mTransferState(state)
        , mType(snapType)
        , mHexDigits(fmt::format("{:08x}", checkpointNum))
        , mLocalPath(snapDir.getName() + "/" + baseName_nogz())
        {}

    FileTransferInfo(T state,
                     TmpDir const& snapDir,
                     std::string const& snapType,
                     std::string const& hexDigits)
        : mTransferState(state)
        , mType(snapType)
        , mHexDigits(hexDigits)
        , mLocalPath(snapDir.getName() + "/" + baseName_nogz())
        {}

    bool getBucketHashName(std::string& hash) const
    {
        if (mHexDigits.size() == 64 && mType == HISTORY_FILE_TYPE_BUCKET)
        {
            hash = mHexDigits;
            return true;
        }
        return false;
    }

    std::string hexDir() const {
        std::regex rx("([[:xdigit:]]{2})([[:xdigit:]]{2})([[:xdigit:]]{2}).*");
        std::smatch sm;
        assert(std::regex_match(mHexDigits, sm, rx));
        return (std::string(sm[1]) + "/" + std::string(sm[2]) + "/" + std::string(sm[3]));
    }

    std::string remoteDir() const {
        return mType + "/" + hexDir();
    }

    T getState() const
    {
        return mTransferState;
    }

    void setState(T state)
    {
        CLOG(DEBUG, "History")
            << "Setting " << baseName_nogz() << " to state " << state;
        mTransferState = state;
    }

    std::string localPath_nogz() const { return mLocalPath; }
    std::string localPath_gz() const { return mLocalPath + ".gz"; }
    std::string baseName_nogz() const { return mType + "-" + mHexDigits + ".xdr"; }
    std::string baseName_gz() const { return baseName_nogz() + ".gz"; }
    std::string remoteName() const { return remoteDir() + "/" + baseName_gz(); }
};

}
