// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketManagerImpl.h"
#include "generated/StellarXDR.h"
#include "main/Application.h"
#include "main/Config.h"
#include "bucket/BucketList.h"
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
#include "medida/counter.h"
#include "medida/meter.h"
#include "medida/timer.h"

namespace stellar
{

std::unique_ptr<BucketManager>
BucketManager::create(Application& app)
{
    return make_unique<BucketManagerImpl>(app);
}

void
BucketManager::dropAll(Application& app)
{
    std::string d = app.getConfig().BUCKET_DIR_PATH;

    if (fs::exists(d))
    {
        CLOG(DEBUG, "Bucket")
            << "Deleting bucket directory: " << d;
        fs::deltree(d);
    }

    if (!fs::exists(d))
    {
        if (!fs::mkdir(d))
        {
            throw std::runtime_error("Unable to create bucket directory: " +
                d);
        }
    }
}

BucketManagerImpl::BucketManagerImpl(Application& app)
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
    , mSharedBucketsSize(
        app.getMetrics().NewCounter({"bucket", "memory", "shared"}))

{
}

const std::string BucketManagerImpl::kLockFilename = "stellar-core.lock";

static std::string
bucketBasename(std::string const& bucketHexHash)
{
    return "bucket-" + bucketHexHash + ".xdr";
}

std::string const&
BucketManagerImpl::getTmpDir()
{
    std::lock_guard<std::recursive_mutex> lock(mBucketMutex);
    if (!mWorkDir)
    {
        TmpDir t = mApp.getTmpDirManager().tmpDir("bucket");
        mWorkDir = make_unique<TmpDir>(std::move(t));
    }
    return mWorkDir->getName();
}

std::string const&
BucketManagerImpl::getBucketDir()
{
    if (!mLockedBucketDir)
    {
        std::string d = mApp.getConfig().BUCKET_DIR_PATH;

        std::string lock = d + "/" + kLockFilename;
        if (fs::exists(lock))
        {
            std::ifstream lockfile(lock);
            std::string pidStr;
            lockfile >> pidStr;
            auto pid = stoi(pidStr);
            if (fs::processExists(pid))
            {
                std::string msg("Found existing lockfile '" + lock + "' and process " + std::to_string(pid) + " is still running.");
                throw std::runtime_error(msg);
            } else
            {
                CLOG(WARNING, "Bucket") << "Ignoring stale lockfile '" << lock << "', process " << pid << " is gone.";
            }
        }

        {
            std::ofstream lockfile(lock, std::ios::trunc);
            lockfile << std::to_string(fs::getCurrentPid()) << std::endl;
        }
        assert(fs::exists(lock));
        mLockedBucketDir = make_unique<std::string>(d);
    }
    return *(mLockedBucketDir);
}

BucketManagerImpl::~BucketManagerImpl()
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
BucketManagerImpl::getBucketList()
{
    return mBucketList;
}

medida::Timer&
BucketManagerImpl::getMergeTimer()
{
    return mBucketSnapMerge;
}

std::shared_ptr<Bucket>
BucketManagerImpl::adoptFileAsBucket(std::string const& filename, uint256 const& hash,
                                     size_t nObjects, size_t nBytes)
{
    std::lock_guard<std::recursive_mutex> lock(mBucketMutex);
    // Check to see if we have an existing bucket (either in-memory or on-disk)
    std::shared_ptr<Bucket> b = getBucketByHash(hash);
    if (b)
    {
        CLOG(DEBUG, "Bucket") << "Deleting bucket file " << filename
                           << " that is redundant with existing bucket";
        std::remove(filename.c_str());
    }
    else
    {
        mBucketObjectInsert.Mark(nObjects);
        mBucketByteInsert.Mark(nBytes);
        std::string basename = bucketBasename(binToHex(hash));
        std::string canonicalName = getBucketDir() + "/" + basename;
        CLOG(DEBUG, "Bucket") << "Adopting bucket file " << filename << " as "
                           << canonicalName;
        if (rename(filename.c_str(), canonicalName.c_str()) != 0)
        {
            std::string err("Failed to rename bucket :");
            err += strerror(errno);
            throw std::runtime_error(err);
        }

        b = std::make_shared<Bucket>(canonicalName, hash);
        {
            mSharedBuckets.insert(std::make_pair(basename, b));
            mSharedBucketsSize.set_count(mSharedBuckets.size());
        }
    }
    assert(b);
    return b;
}

std::shared_ptr<Bucket>
BucketManagerImpl::getBucketByHash(uint256 const& hash)
{
    std::lock_guard<std::recursive_mutex> lock(mBucketMutex);
    if (isZero(hash))
    {
        return std::make_shared<Bucket>();
    }
    std::string basename = bucketBasename(binToHex(hash));
    auto i = mSharedBuckets.find(basename);
    if (i != mSharedBuckets.end())
    {
        CLOG(TRACE, "Bucket")
            << "BucketManager::getBucketByHash("
            << binToHex(hash) << ") found bucket "
            << i->second->getFilename();
        return i->second;
    }
    std::string canonicalName = getBucketDir() + "/" + basename;
    if (fs::exists(canonicalName))
    {
        CLOG(TRACE, "Bucket")
            << "BucketManager::getBucketByHash("
            << binToHex(hash) << ") found no bucket, making new one";
        auto p = std::make_shared<Bucket>(canonicalName, hash);
        mSharedBuckets.insert(std::make_pair(basename, p));
        mSharedBucketsSize.set_count(mSharedBuckets.size());
        return p;
    }
    return std::shared_ptr<Bucket>();
}

void
BucketManagerImpl::forgetUnreferencedBuckets()
{

    std::lock_guard<std::recursive_mutex> lock(mBucketMutex);
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

        // Only drop buckets if the bucketlist has forgotten them _and_
        // no other in-progress structures (worker threads, shadow lists)
        // have references to them, just us. It's ok to retain a few too
        // many buckets, a little longer than necessary.
        //
        // This conservatism is important because we want to enforce that only
        // one bucket ever exists in memory with a given filename, and that
        // we're the first and last to know about it. Otherwise buckets might
        // race on deleting the underlying file from one another.

        if (referenced.find(j->first) == referenced.end() &&
            j->second.use_count() == 1)
        {
            CLOG(TRACE, "Bucket")
                << "BucketManager::forgetUnreferencedBuckets dropping "
                << j->second->getFilename();
            j->second->setRetain(false);
            mSharedBuckets.erase(j);
            mSharedBucketsSize.set_count(mSharedBuckets.size());
        }
        else
        {
            j->second->setRetain(true);
        }
    }
}

void
BucketManagerImpl::addBatch(Application& app, uint32_t currLedger,
                    std::vector<LedgerEntry> const& liveEntries,
                    std::vector<LedgerKey> const& deadEntries)
{
    auto timer = mBucketAddBatch.TimeScope();
    mBucketList.addBatch(app, currLedger, liveEntries, deadEntries);
}

// updates the given LedgerHeader to reflect the current state of the bucket
// list
void
BucketManagerImpl::snapshotLedger(LedgerHeader& currentHeader)
{
    currentHeader.bucketListHash = mBucketList.getHash();
}

std::vector<std::string>
BucketManagerImpl::checkForMissingBucketsFiles(HistoryArchiveState const& has)
{
    std::vector<std::string> buckets;
    for (size_t i = 0; i < BucketList::kNumLevels; ++i)
    {
        auto snap = bucketBasename(has.currentBuckets.at(i).snap);
        buckets.push_back(has.currentBuckets.at(i).curr);
        buckets.push_back(has.currentBuckets.at(i).snap);
    }

    std::vector<std::string> result;
    std::copy_if(buckets.begin(), buckets.end(), std::back_inserter(result), [&](std::string b) {
        auto filename = getBucketDir() + "/" + bucketBasename(b);
        return !isZero(hexToBin256(b)) && !fs::exists(filename);
    });

    return result;
}

void
BucketManagerImpl::assumeState(HistoryArchiveState const& has)
{
    for (size_t i = 0; i < BucketList::kNumLevels; ++i)
    {
        auto curr = getBucketByHash(hexToBin256(has.currentBuckets.at(i).curr));
        auto snap = getBucketByHash(hexToBin256(has.currentBuckets.at(i).snap));
        if (!(curr && snap))
        {
            throw std::runtime_error(
                "Missing bucket files while assuming saved BucketList state");
        }
        mBucketList.getLevel(i).setCurr(curr);
        mBucketList.getLevel(i).setSnap(snap);
    }
    mBucketList.restartMerges(mApp, has.currentLedger);
}
}
