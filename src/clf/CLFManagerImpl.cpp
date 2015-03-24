// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "clf/CLFManagerImpl.h"
#include "generated/StellarXDR.h"
#include "main/Application.h"
#include "main/Config.h"
#include "clf/BucketList.h"
#include "history/HistoryManager.h"
#include "util/Fs.h"
#include "util/make_unique.h"
#include "util/TmpDir.h"
#include "util/Logging.h"
#include "util/types.h"
#include "crypto/Hex.h"
#include <fstream>
#include <map>
#include <set>

#include "medida/metrics_registry.h"
#include "medida/meter.h"
#include "medida/timer.h"

namespace stellar
{

std::unique_ptr<CLFManager>
CLFManager::create(Application& app)
{
    return make_unique<CLFManagerImpl>(app);
}

CLFManagerImpl::CLFManagerImpl(Application& app)
    : mApp(app)
    , mWorkDir(nullptr)
    , mLockedBucketDir(nullptr)
    , mBucketObjectInsert(app.getMetrics().NewMeter(
                              {"bucket", "object", "insert"}, "object"))
    , mBucketByteInsert(
        app.getMetrics().NewMeter({"bucket", "byte", "insert"}, "byte"))
    , mBucketAddBatch(app.getMetrics().NewTimer({"bucket", "batch", "add"}))
    , mBucketSnapMerge(
        app.getMetrics().NewTimer({"bucket", "snap", "merge"}))
{
}

const std::string CLFManagerImpl::kLockFilename = "stellard.lock";

static std::string
bucketBasename(std::string const& bucketHexHash)
{
    return "bucket-" + bucketHexHash + ".xdr";
}

std::string const&
CLFManagerImpl::getTmpDir()
{
    if (!mWorkDir)
    {
        TmpDir t = mApp.getTmpDirMaster().tmpDir("clf");
        mWorkDir = make_unique<TmpDir>(std::move(t));
    }
    return mWorkDir->getName();
}

std::string const&
CLFManagerImpl::getBucketDir()
{
    if (!mLockedBucketDir)
    {
        std::string d = mApp.getConfig().BUCKET_DIR_PATH;

        if (mApp.getConfig().START_NEW_NETWORK)
        {
            if (fs::exists(d))
            {
                CLOG(DEBUG, "CLF")
                    << "Deleting bucket directory for new network: " << d;
                fs::deltree(d);
            }
        }

        if (!fs::exists(d))
        {
            if (!fs::mkdir(d))
            {
                throw std::runtime_error("Unable to create bucket directory: " +
                                         d);
            }
        }

        std::string lock = d + "/" + kLockFilename;
        if (fs::exists(lock))
        {
            std::string msg("Found existing lockfile '");
            msg += lock;
            msg += "'";
            throw std::runtime_error(msg);
        }

        assert(!fs::exists(lock));
        {
            std::ofstream lockfile(lock);
            lockfile << 1;
        }
        assert(fs::exists(lock));
        mLockedBucketDir = make_unique<std::string>(d);
    }
    return *(mLockedBucketDir);
}

CLFManagerImpl::~CLFManagerImpl()
{
    if (mLockedBucketDir)
    {
        std::string d = mApp.getConfig().BUCKET_DIR_PATH;
        std::string lock = d + "/" + kLockFilename;
        assert(fs::exists(lock));
        std::remove(lock.c_str());
        assert(!fs::exists(lock));
    }
}

BucketList&
CLFManagerImpl::getBucketList()
{
    return mBucketList;
}

medida::Timer&
CLFManagerImpl::getMergeTimer()
{
    return mBucketSnapMerge;
}

std::shared_ptr<Bucket>
CLFManagerImpl::adoptFileAsBucket(std::string const& filename, uint256 const& hash,
                             size_t nObjects, size_t nBytes)
{
    std::lock_guard<std::mutex> lock(mBucketMutex);
    mBucketObjectInsert.Mark(nObjects);
    mBucketByteInsert.Mark(nBytes);
    std::string basename = bucketBasename(binToHex(hash));
    std::shared_ptr<Bucket> b;
    if (mSharedBuckets.find(basename) == mSharedBuckets.end())
    {
        // We do not yet have this file under its canonical name,
        // so we'll move it into place.
        std::string canonicalName = getBucketDir() + "/" + basename;
        CLOG(DEBUG, "CLF") << "Adopting bucket file " << filename << " as "
                           << canonicalName;
        if (rename(filename.c_str(), canonicalName.c_str()) != 0)
        {
            throw std::runtime_error("Failed to rename bucket");
        }
        mSharedBuckets[basename] =
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
    return mSharedBuckets[basename];
}

std::shared_ptr<Bucket>
CLFManagerImpl::getBucketByHash(uint256 const& hash) const
{
    if (isZero(hash))
    {
        return std::make_shared<Bucket>();
    }
    std::lock_guard<std::mutex> lock(mBucketMutex);
    std::string basename = bucketBasename(binToHex(hash));
    auto i = mSharedBuckets.find(basename);
    if (i != mSharedBuckets.end())
    {
        return i->second;
    }
    return std::shared_ptr<Bucket>();
}

void
CLFManagerImpl::forgetUnreferencedBuckets()
{
    std::lock_guard<std::mutex> lock(mBucketMutex);
    std::set<std::string> referenced;
    for (size_t i = 0; i < BucketList::kNumLevels; ++i)
    {
        auto const& level = mBucketList.getLevel(i);
        uint256 hashes[2] = {level.getCurr()->getHash(),
                             level.getSnap()->getHash()};
        for (auto const& hash : hashes)
        {
            std::string basename = bucketBasename(binToHex(hash));
            referenced.insert(basename);
        }
    }

    for (auto i = mSharedBuckets.begin();
         i != mSharedBuckets.end();)
    {
        // Standard says map iterators other than the one you're erasing remain
        // valid.
        auto j = i;
        ++i;
        if (referenced.find(j->first) == referenced.end())
        {
            j->second->setRetain(false);
            mSharedBuckets.erase(j);
        }
        else
        {
            j->second->setRetain(true);
        }
    }
}

void
CLFManagerImpl::addBatch(Application& app, uint32_t currLedger,
                    std::vector<LedgerEntry> const& liveEntries,
                    std::vector<LedgerKey> const& deadEntries)
{
    auto timer = mBucketAddBatch.TimeScope();
    mBucketList.addBatch(app, currLedger, liveEntries, deadEntries);
}

// updates the given LedgerHeader to reflect the current state of the bucket
// list
void
CLFManagerImpl::snapshotLedger(LedgerHeader& currentHeader)
{
    currentHeader.clfHash = mBucketList.getHash();
}
}
