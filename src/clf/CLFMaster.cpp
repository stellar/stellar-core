// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "generated/StellarXDR.h"
#include "main/Application.h"
#include "main/Config.h"
#include "clf/CLFMaster.h"
#include "clf/BucketList.h"
#include "history/HistoryMaster.h"
#include "util/make_unique.h"
#include "util/TmpDir.h"
#include "util/Logging.h"
#include "crypto/Hex.h"
#include <fstream>
#include <map>
#include <set>

namespace stellar
{

class CLFMaster::Impl
{
public:
    Application& mApp;
    LedgerHeader mHeader;
    BucketList mBucketList;
    std::unique_ptr<TmpDir> mWorkDir;
    std::map<std::string, std::shared_ptr<Bucket>> mSharedBuckets;
    std::mutex mBucketMutex;
    std::unique_ptr<std::string> lockedBucketDir;
    Impl(Application &app)
        : mApp(app)
        , mWorkDir(nullptr)
        , lockedBucketDir(nullptr)
        {}
};

const std::string
CLFMaster::kLockFilename = "stellard.lock";

CLFMaster::CLFMaster(Application& app)
    : mImpl(make_unique<Impl>(app))
{
}

std::string const&
CLFMaster::getTmpDir()
{
    if (!mImpl->mWorkDir)
    {
        TmpDir t = mImpl->mApp.getTmpDirMaster().tmpDir("clf");
        mImpl->mWorkDir = make_unique<TmpDir>(std::move(t));
    }
    return mImpl->mWorkDir->getName();
}

std::string const&
CLFMaster::getBucketDir()
{
    if (!mImpl->lockedBucketDir)
    {
        std::string d = mImpl->mApp.getConfig().BUCKET_DIR_PATH;

        if (mImpl->mApp.getConfig().START_NEW_NETWORK)
        {
            if (TmpDir::exists(d))
            {
                CLOG(INFO, "CLF") << "Deleting bucket directory for new network: " << d;
                TmpDir::deltree(d);
            }
        }

        if (!TmpDir::exists(d))
        {
            if (!TmpDir::mkdir(d))
            {
                throw std::runtime_error("Unable to create bucket directory: " + d);
            }
        }

        std::string lock = d + "/" + kLockFilename;
        if (TmpDir::exists(lock))
        {
            std::string msg("Found existing lockfile '");
            msg += lock;
            msg += "'";
            throw std::runtime_error(msg);
        }

        assert(!TmpDir::exists(lock));
        {
            std::ofstream lockfile(lock);
            lockfile << 1;
        }
        assert(TmpDir::exists(lock));
        mImpl->lockedBucketDir = make_unique<std::string>(d);
    }
    return *(mImpl->lockedBucketDir);
}

CLFMaster::~CLFMaster()
{
    if (mImpl && mImpl->lockedBucketDir)
    {
        std::string d = mImpl->mApp.getConfig().BUCKET_DIR_PATH;
        std::string lock = d + "/" + kLockFilename;
        assert(TmpDir::exists(lock));
        std::remove(lock.c_str());
        assert(!TmpDir::exists(lock));
    }
}

LedgerHeader const&
CLFMaster::getHeader()
{
    return mImpl->mHeader;
}

BucketList &
CLFMaster::getBucketList()
{
    return mImpl->mBucketList;
}


std::shared_ptr<Bucket>
CLFMaster::adoptFileAsBucket(std::string const& filename,
                             uint256 const& hash)
{
    std::lock_guard<std::mutex> lock(mImpl->mBucketMutex);
    std::string basename = HistoryMaster::bucketBasename(binToHex(hash));
    std::shared_ptr<Bucket> b;
    if (mImpl->mSharedBuckets.find(basename) ==
        mImpl->mSharedBuckets.end())
    {
        // We do not yet have this file under its canonical name,
        // so we'll move it into place.
        std::string canonicalName = getBucketDir() + "/" + basename;
        CLOG(DEBUG, "CLF") << "Adopting bucket file " << filename
                           << " as " << canonicalName;
        if (rename(filename.c_str(), canonicalName.c_str()) != 0)
        {
            throw std::runtime_error("Failed to rename bucket");
        }
        mImpl->mSharedBuckets[basename] =
            std::make_shared<Bucket>(canonicalName, hash);
    }
    else
    {
        // We already have a bucket with this hash, so just kill the
        // source file.
        CLOG(DEBUG, "CLF") << "Deleting bucket file " << filename
                          << " that is redundant with existing bucket";
        std::remove(filename.c_str());
    }
    return mImpl->mSharedBuckets[basename];
}

void
CLFMaster::forgetUnreferencedBuckets()
{
    std::lock_guard<std::mutex> lock(mImpl->mBucketMutex);
    std::set<std::string> referenced;
    for (size_t i = 0; i < BucketList::kNumLevels; ++i)
    {
        auto const& level = mImpl->mBucketList.getLevel(i);
        uint256 hashes[2] = { level.getCurr()->getHash(), level.getSnap()->getHash() };
        for (auto const& hash : hashes)
        {
            std::string basename = HistoryMaster::bucketBasename(binToHex(hash));
            referenced.insert(basename);
        }
    }

    for (auto i = mImpl->mSharedBuckets.begin(); i != mImpl->mSharedBuckets.end();)
    {
        // Standard says map iterators other than the one you're erasing remain valid.
        auto j = i;
        ++i;
        if (referenced.find(j->first) == referenced.end())
        {
            mImpl->mSharedBuckets.erase(j);
        }
    }
}


}
