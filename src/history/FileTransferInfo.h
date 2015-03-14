#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "clf/Bucket.h"
#include "crypto/Hex.h"
#include "util/Fs.h"
#include "util/Logging.h"
#include "util/TmpDir.h"
#include <string>

namespace stellar
{

extern char const* HISTORY_FILE_TYPE_BUCKET;
extern char const* HISTORY_FILE_TYPE_LEDGER;
extern char const* HISTORY_FILE_TYPE_TRANSACTIONS;
extern char const* HISTORY_FILE_TYPE_RESULTS;

template <typename T>
class
FileTransferInfo
{
    T mTransferState;
    std::string mType;
    std::string mHexDigits;
    std::string mLocalPath;
    std::string mSuffix;

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
        , mHexDigits(fs::hexStr(checkpointNum))
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

    std::string baseName_nogz() const { return fs::baseName(mType, mHexDigits, "xdr"); }
    std::string baseName_gz() const { return baseName_nogz() + ".gz"; }

    std::string remoteDir() const { return fs::remoteDir(mType, mHexDigits); }
    std::string remoteName() const { return fs::remoteName(mType, mHexDigits, "xdr"); }
};

}
