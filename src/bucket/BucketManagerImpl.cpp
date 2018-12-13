// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketManagerImpl.h"
#include "bucket/BucketList.h"
#include "crypto/Hex.h"
#include "history/HistoryManager.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/StellarXDR.h"
#include "util/Fs.h"
#include "util/LogSlowExecution.h"
#include "util/Logging.h"
#include "util/TmpDir.h"
#include "util/types.h"
#include <fstream>
#include <map>
#include <regex>
#include <set>

#include "medida/counter.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "medida/timer.h"

namespace stellar
{

std::unique_ptr<BucketManager>
BucketManager::create(Application& app)
{
    auto bucketManagerPtr = std::make_unique<BucketManagerImpl>(app);
    bucketManagerPtr->initialize();
    return bucketManagerPtr;
}

void
BucketManagerImpl::initialize()
{
    std::string d = mApp.getConfig().BUCKET_DIR_PATH;

    if (!fs::exists(d))
    {
        if (!fs::mkpath(d))
        {
            throw std::runtime_error("Unable to create bucket directory: " + d);
        }
    }

    // Acquire exclusive lock on `buckets` folder
    std::string lock = d + "/" + kLockFilename;

    // there are many reasons the lock can fail so let lockFile throw
    // directly for more clear error messages since we end up just raising
    // a runtime exception anyway
    fs::lockFile(lock);

    mLockedBucketDir = std::make_unique<std::string>(d);
    mTmpDirManager = std::make_unique<TmpDirManager>(d + "/tmp");
}

void
BucketManagerImpl::dropAll()
{
    std::string d = mApp.getConfig().BUCKET_DIR_PATH;

    if (fs::exists(d))
    {
        CLOG(DEBUG, "Bucket") << "Deleting bucket directory: " << d;
        cleanDir();
        fs::deltree(d);
    }

    initialize();
}

TmpDirManager&
BucketManagerImpl::getTmpDirManager()
{
    return *mTmpDirManager;
}

BucketManagerImpl::BucketManagerImpl(Application& app)
    : mApp(app)
    , mTmpDirManager(nullptr)
    , mWorkDir(nullptr)
    , mLockedBucketDir(nullptr)
    , mBucketObjectInsertBatch(app.getMetrics().NewMeter(
          {"bucket", "batch", "objectsadded"}, "object"))
    , mBucketAddBatch(app.getMetrics().NewTimer({"bucket", "batch", "addtime"}))
    , mBucketSnapMerge(app.getMetrics().NewTimer({"bucket", "snap", "merge"}))
    , mSharedBucketsSize(
          app.getMetrics().NewCounter({"bucket", "memory", "shared"}))

{
}

const std::string BucketManagerImpl::kLockFilename = "stellar-core.lock";

namespace
{
std::string
bucketBasename(std::string const& bucketHexHash)
{
    return "bucket-" + bucketHexHash + ".xdr";
}

bool
isBucketFile(std::string const& name)
{
    static std::regex re("^bucket-[a-z0-9]{64}\\.xdr(\\.gz)?$");
    return std::regex_match(name, re);
};

uint256
extractFromFilename(std::string const& name)
{
    return hexToBin256(name.substr(7, 64));
};
}

std::string
BucketManagerImpl::bucketFilename(std::string const& bucketHexHash)
{
    std::string basename = bucketBasename(bucketHexHash);
    return getBucketDir() + "/" + basename;
}

std::string
BucketManagerImpl::bucketFilename(Hash const& hash)
{
    return bucketFilename(binToHex(hash));
}

std::string const&
BucketManagerImpl::getTmpDir()
{
    std::lock_guard<std::recursive_mutex> lock(mBucketMutex);
    if (!mWorkDir)
    {
        TmpDir t = mTmpDirManager->tmpDir("bucket");
        mWorkDir = std::make_unique<TmpDir>(std::move(t));
    }
    return mWorkDir->getName();
}

std::string const&
BucketManagerImpl::getBucketDir()
{
    return *(mLockedBucketDir);
}

BucketManagerImpl::~BucketManagerImpl()
{
    cleanDir();
}

void
BucketManagerImpl::cleanDir()
{
    if (mLockedBucketDir)
    {
        std::string d = mApp.getConfig().BUCKET_DIR_PATH;
        std::string lock = d + "/" + kLockFilename;
        assert(fs::exists(lock));
        fs::unlockFile(lock);
        mLockedBucketDir.reset();
    }
    mWorkDir.reset();
    mTmpDirManager.reset();
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
BucketManagerImpl::adoptFileAsBucket(std::string const& filename,
                                     uint256 const& hash, size_t nObjects,
                                     size_t nBytes)
{
    std::lock_guard<std::recursive_mutex> lock(mBucketMutex);
    // Check to see if we have an existing bucket (either in-memory or on-disk)
    std::shared_ptr<Bucket> b = getBucketByHash(hash);
    if (b)
    {
        CLOG(DEBUG, "Bucket") << "Deleting bucket file " << filename
                              << " that is redundant with existing bucket";
        {
            auto timer = LogSlowExecution("Delete redundant bucket");
            std::remove(filename.c_str());
        }
    }
    else
    {
        std::string canonicalName = bucketFilename(hash);
        CLOG(DEBUG, "Bucket")
            << "Adopting bucket file " << filename << " as " << canonicalName;
        if (rename(filename.c_str(), canonicalName.c_str()) != 0)
        {
            std::string err("Failed to rename bucket :");
            err += strerror(errno);
            // it seems there is a race condition with external systems
            // retry after sleeping for a second works around the problem
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (rename(filename.c_str(), canonicalName.c_str()) != 0)
            {
                // if rename fails again, surface the original error
                throw std::runtime_error(err);
            }
        }

        b = std::make_shared<Bucket>(canonicalName, hash);
        {
            mSharedBuckets.insert(std::make_pair(hash, b));
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
    auto i = mSharedBuckets.find(hash);
    if (i != mSharedBuckets.end())
    {
        CLOG(TRACE, "Bucket")
            << "BucketManager::getBucketByHash(" << binToHex(hash)
            << ") found bucket " << i->second->getFilename();
        return i->second;
    }
    std::string canonicalName = bucketFilename(hash);
    if (fs::exists(canonicalName))
    {
        CLOG(TRACE, "Bucket")
            << "BucketManager::getBucketByHash(" << binToHex(hash)
            << ") found no bucket, making new one";
        auto p = std::make_shared<Bucket>(canonicalName, hash);
        mSharedBuckets.insert(std::make_pair(hash, p));
        mSharedBucketsSize.set_count(mSharedBuckets.size());
        return p;
    }
    return std::shared_ptr<Bucket>();
}

std::set<Hash>
BucketManagerImpl::getReferencedBuckets() const
{
    std::set<Hash> referenced;
    // retain current bucket list
    for (uint32_t i = 0; i < BucketList::kNumLevels; ++i)
    {
        auto const& level = mBucketList.getLevel(i);
        auto rit = referenced.insert(level.getCurr()->getHash());
        if (rit.second)
        {
            CLOG(TRACE, "Bucket")
                << binToHex(*rit.first) << " referenced by bucket list";
        }
        rit = referenced.insert(level.getSnap()->getHash());
        if (rit.second)
        {
            CLOG(TRACE, "Bucket")
                << binToHex(*rit.first) << " referenced by bucket list";
        }
        for (auto const& h : level.getNext().getHashes())
        {
            rit = referenced.insert(hexToBin256(h));
            if (rit.second)
            {
                CLOG(TRACE, "Bucket") << h << " referenced by bucket list";
            }
        }
    }
    // retain any bucket referenced by the last closed ledger as recorded in the
    // database (as merge complete, the bucket list drifts from that state)
    auto lclHas = mApp.getLedgerManager().getLastClosedLedgerHAS();
    auto lclBuckets = lclHas.allBuckets();
    for (auto const& h : lclBuckets)
    {
        auto rit = referenced.insert(hexToBin256(h));
        if (rit.second)
        {
            CLOG(TRACE, "Bucket") << h << " referenced by LCL";
        }
    }

    // retain buckets that are referenced by a state in the publish queue.
    auto pub = mApp.getHistoryManager().getBucketsReferencedByPublishQueue();
    {
        for (auto const& h : pub)
        {
            auto rit = referenced.insert(hexToBin256(h));
            if (rit.second)
            {
                CLOG(TRACE, "Bucket") << h << " referenced by publish queue";
            }
        }
    }

    return referenced;
}

void
BucketManagerImpl::cleanupStaleFiles()
{
    if (mApp.getConfig().DISABLE_BUCKET_GC)
    {
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(mBucketMutex);
    auto referenced = getReferencedBuckets();
    std::transform(std::begin(mSharedBuckets), std::end(mSharedBuckets),
                   std::inserter(referenced, std::end(referenced)),
                   [](std::pair<Hash, std::shared_ptr<Bucket>> const& p) {
                       return p.first;
                   });

    for (auto f : fs::findfiles(getBucketDir(), isBucketFile))
    {
        auto hash = extractFromFilename(f);
        if (referenced.find(hash) == std::end(referenced))
        {
            // we don't care about failure here
            // if removing file failed one time, it may not fail when this is
            // called again
            auto fullName = getBucketDir() + "/" + f;
            std::remove(fullName.c_str());
        }
    }
}

void
BucketManagerImpl::forgetUnreferencedBuckets()
{
    std::lock_guard<std::recursive_mutex> lock(mBucketMutex);
    auto referenced = getReferencedBuckets();

    for (auto i = mSharedBuckets.begin(); i != mSharedBuckets.end();)
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
            auto filename = j->second->getFilename();
            CLOG(TRACE, "Bucket")
                << "BucketManager::forgetUnreferencedBuckets dropping "
                << filename;
            if (!filename.empty() && !mApp.getConfig().DISABLE_BUCKET_GC)
            {
                CLOG(TRACE, "Bucket") << "removing bucket file: " << filename;
                std::remove(filename.c_str());
                auto gzfilename = filename + ".gz";
                std::remove(gzfilename.c_str());
            }
            mSharedBuckets.erase(j);
        }
    }
    mSharedBucketsSize.set_count(mSharedBuckets.size());
}

void
BucketManagerImpl::addBatch(Application& app, uint32_t currLedger,
                            std::vector<LedgerEntry> const& liveEntries,
                            std::vector<LedgerKey> const& deadEntries)
{
    auto timer = mBucketAddBatch.TimeScope();
    mBucketObjectInsertBatch.Mark(liveEntries.size());
    mBucketList.addBatch(app, currLedger, liveEntries, deadEntries);
}

// updates the given LedgerHeader to reflect the current state of the bucket
// list
void
BucketManagerImpl::snapshotLedger(LedgerHeader& currentHeader)
{
    currentHeader.bucketListHash = mBucketList.getHash();
    calculateSkipValues(currentHeader);
}

void
BucketManagerImpl::calculateSkipValues(LedgerHeader& currentHeader)
{

    if ((currentHeader.ledgerSeq % SKIP_1) == 0)
    {
        int v = currentHeader.ledgerSeq - SKIP_1;
        if (v > 0 && (v % SKIP_2) == 0)
        {
            v = currentHeader.ledgerSeq - SKIP_2 - SKIP_1;
            if (v > 0 && (v % SKIP_3) == 0)
            {
                v = currentHeader.ledgerSeq - SKIP_3 - SKIP_2 - SKIP_1;
                if (v > 0 && (v % SKIP_4) == 0)
                {

                    currentHeader.skipList[3] = currentHeader.skipList[2];
                }
                currentHeader.skipList[2] = currentHeader.skipList[1];
            }
            currentHeader.skipList[1] = currentHeader.skipList[0];
        }
        currentHeader.skipList[0] = currentHeader.bucketListHash;
    }
}

std::vector<std::string>
BucketManagerImpl::checkForMissingBucketsFiles(HistoryArchiveState const& has)
{
    std::vector<std::string> buckets = has.allBuckets();
    std::vector<std::string> result;
    std::copy_if(buckets.begin(), buckets.end(), std::back_inserter(result),
                 [&](std::string b) {
                     auto filename = bucketFilename(b);
                     return !isZero(hexToBin256(b)) && !fs::exists(filename);
                 });

    return result;
}

void
BucketManagerImpl::assumeState(HistoryArchiveState const& has)
{
    for (uint32_t i = 0; i < BucketList::kNumLevels; ++i)
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
        mBucketList.getLevel(i).setNext(has.currentBuckets.at(i).next);
    }

    mBucketList.restartMerges(mApp);
    cleanupStaleFiles();
}

void
BucketManagerImpl::shutdown()
{
    // forgetUnreferencedBuckets does what we want - it retains needed buckets
    forgetUnreferencedBuckets();
}
}
