// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketManagerImpl.h"
#include "bucket/Bucket.h"
#include "bucket/BucketInputIterator.h"
#include "bucket/BucketList.h"
#include "bucket/BucketOutputIterator.h"
#include "crypto/Hex.h"
#include "history/HistoryManager.h"
#include "historywork/VerifyBucketWork.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/StellarXDR.h"
#include "util/Fs.h"
#include "util/GlobalChecks.h"
#include "util/LogSlowExecution.h"
#include "util/Logging.h"
#include "util/TmpDir.h"
#include "util/types.h"
#include <filesystem>
#include <fmt/chrono.h>
#include <fmt/format.h>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <thread>

#include "medida/counter.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "medida/timer.h"
#include "work/WorkScheduler.h"
#include "xdrpp/printer.h"
#include <Tracy.hpp>

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
    ZoneScoped;
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
    try
    {
        fs::lockFile(lock);
    }
    catch (std::exception const& e)
    {
        throw std::runtime_error(fmt::format(
            FMT_STRING("{}. This can be caused by access rights issues or "
                       "another stellar-core process already running"),
            e.what()));
    }

    mLockedBucketDir = std::make_unique<std::string>(d);
    mTmpDirManager = std::make_unique<TmpDirManager>(d + "/tmp");

    if (mApp.getConfig().MODE_ENABLES_BUCKETLIST)
    {
        mBucketList = std::make_unique<BucketList>();
    }
}

void
BucketManagerImpl::dropAll()
{
    ZoneScoped;
    deleteEntireBucketDir();
    initialize();
}

TmpDirManager&
BucketManagerImpl::getTmpDirManager()
{
    return *mTmpDirManager;
}

BucketManagerImpl::BucketManagerImpl(Application& app)
    : mApp(app)
    , mBucketList(nullptr)
    , mTmpDirManager(nullptr)
    , mWorkDir(nullptr)
    , mLockedBucketDir(nullptr)
    , mBucketObjectInsertBatch(app.getMetrics().NewMeter(
          {"bucket", "batch", "objectsadded"}, "object"))
    , mBucketAddBatch(app.getMetrics().NewTimer({"bucket", "batch", "addtime"}))
    , mBucketSnapMerge(app.getMetrics().NewTimer({"bucket", "snap", "merge"}))
    , mSharedBucketsSize(
          app.getMetrics().NewCounter({"bucket", "memory", "shared"}))
    , mBucketListDBQueryMeter(app.getMetrics().NewMeter(
          {"bucketlistDB", "query", "loads"}, "query"))
    , mBucketListDBBloomMisses(app.getMetrics().NewMeter(
          {"bucketlistDB", "bloom", "misses"}, "bloom"))
    , mBucketListDBBloomLookups(app.getMetrics().NewMeter(
          {"bucketlistDB", "bloom", "lookups"}, "bloom"))
    // Minimal DB is stored in the buckets dir, so delete it only when
    // mode does not use minimal DB
    , mDeleteEntireBucketDirInDtor(
          app.getConfig().isInMemoryModeWithoutMinimalDB())
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
    ZoneScoped;
    std::lock_guard<std::recursive_mutex> lock(mBucketMutex);
    if (!mWorkDir)
    {
        TmpDir t = mTmpDirManager->tmpDir("bucket");
        mWorkDir = std::make_unique<TmpDir>(std::move(t));
    }
    return mWorkDir->getName();
}

std::string const&
BucketManagerImpl::getBucketDir() const
{
    return *(mLockedBucketDir);
}

BucketManagerImpl::~BucketManagerImpl()
{
    ZoneScoped;
    if (mDeleteEntireBucketDirInDtor)
    {
        deleteEntireBucketDir();
    }
    else
    {
        deleteTmpDirAndUnlockBucketDir();
    }
}

void
BucketManagerImpl::deleteEntireBucketDir()
{
    ZoneScoped;
    std::string d = mApp.getConfig().BUCKET_DIR_PATH;
    if (fs::exists(d))
    {
        // First clean out the contents of the tmpdir, as usual.
        deleteTmpDirAndUnlockBucketDir();

        // Then more seriously delete _all the buckets_, even live
        // ones that represent the canonical state of the ledger.
        //
        // Should only happen on new-db or in-memory-replay shutdown.
        CLOG_DEBUG(Bucket, "Deleting bucket directory: {}", d);
        fs::deltree(d);
    }
}

void
BucketManagerImpl::deleteTmpDirAndUnlockBucketDir()
{
    ZoneScoped;

    // First do fs::deltree on $BUCKET_DIR_PATH/tmp/bucket
    //
    // (which should just be bucket merges-in-progress and such)
    mWorkDir.reset();

    // Then do fs::deltree on $BUCKET_DIR_PATH/tmp
    //
    // (which also contains files from other subsystems, like history)
    mTmpDirManager.reset();

    // Then delete the lockfile $BUCKET_DIR_PATH/stellar-core.lock
    if (mLockedBucketDir)
    {
        std::string d = mApp.getConfig().BUCKET_DIR_PATH;
        std::string lock = d + "/" + kLockFilename;
        releaseAssert(fs::exists(lock));
        fs::unlockFile(lock);
        mLockedBucketDir.reset();
    }
}

BucketList&
BucketManagerImpl::getBucketList()
{
    releaseAssertOrThrow(mApp.getConfig().MODE_ENABLES_BUCKETLIST);
    return *mBucketList;
}

medida::Timer&
BucketManagerImpl::getMergeTimer()
{
    return mBucketSnapMerge;
}

MergeCounters
BucketManagerImpl::readMergeCounters()
{
    std::lock_guard<std::recursive_mutex> lock(mBucketMutex);
    return mMergeCounters;
}

MergeCounters&
MergeCounters::operator+=(MergeCounters const& delta)
{
    mPreInitEntryProtocolMerges += delta.mPreInitEntryProtocolMerges;
    mPostInitEntryProtocolMerges += delta.mPostInitEntryProtocolMerges;

    mRunningMergeReattachments += delta.mRunningMergeReattachments;
    mFinishedMergeReattachments += delta.mFinishedMergeReattachments;

    mPreShadowRemovalProtocolMerges += delta.mPreShadowRemovalProtocolMerges;
    mPostShadowRemovalProtocolMerges += delta.mPostShadowRemovalProtocolMerges;

    mNewMetaEntries += delta.mNewMetaEntries;
    mNewInitEntries += delta.mNewInitEntries;
    mNewLiveEntries += delta.mNewLiveEntries;
    mNewDeadEntries += delta.mNewDeadEntries;
    mOldMetaEntries += delta.mOldMetaEntries;
    mOldInitEntries += delta.mOldInitEntries;
    mOldLiveEntries += delta.mOldLiveEntries;
    mOldDeadEntries += delta.mOldDeadEntries;

    mOldEntriesDefaultAccepted += delta.mOldEntriesDefaultAccepted;
    mNewEntriesDefaultAccepted += delta.mNewEntriesDefaultAccepted;
    mNewInitEntriesMergedWithOldDead += delta.mNewInitEntriesMergedWithOldDead;
    mOldInitEntriesMergedWithNewLive += delta.mOldInitEntriesMergedWithNewLive;
    mOldInitEntriesMergedWithNewDead += delta.mOldInitEntriesMergedWithNewDead;
    mNewEntriesMergedWithOldNeitherInit +=
        delta.mNewEntriesMergedWithOldNeitherInit;

    mShadowScanSteps += delta.mShadowScanSteps;
    mMetaEntryShadowElisions += delta.mMetaEntryShadowElisions;
    mLiveEntryShadowElisions += delta.mLiveEntryShadowElisions;
    mInitEntryShadowElisions += delta.mInitEntryShadowElisions;
    mDeadEntryShadowElisions += delta.mDeadEntryShadowElisions;

    mOutputIteratorTombstoneElisions += delta.mOutputIteratorTombstoneElisions;
    mOutputIteratorBufferUpdates += delta.mOutputIteratorBufferUpdates;
    mOutputIteratorActualWrites += delta.mOutputIteratorActualWrites;
    return *this;
}

bool
MergeCounters::operator==(MergeCounters const& other) const
{
    return (
        mPreInitEntryProtocolMerges == other.mPreInitEntryProtocolMerges &&
        mPostInitEntryProtocolMerges == other.mPostInitEntryProtocolMerges &&

        mRunningMergeReattachments == other.mRunningMergeReattachments &&
        mFinishedMergeReattachments == other.mFinishedMergeReattachments &&

        mNewMetaEntries == other.mNewMetaEntries &&
        mNewInitEntries == other.mNewInitEntries &&
        mNewLiveEntries == other.mNewLiveEntries &&
        mNewDeadEntries == other.mNewDeadEntries &&
        mOldMetaEntries == other.mOldMetaEntries &&
        mOldInitEntries == other.mOldInitEntries &&
        mOldLiveEntries == other.mOldLiveEntries &&
        mOldDeadEntries == other.mOldDeadEntries &&

        mOldEntriesDefaultAccepted == other.mOldEntriesDefaultAccepted &&
        mNewEntriesDefaultAccepted == other.mNewEntriesDefaultAccepted &&
        mNewInitEntriesMergedWithOldDead ==
            other.mNewInitEntriesMergedWithOldDead &&
        mOldInitEntriesMergedWithNewLive ==
            other.mOldInitEntriesMergedWithNewLive &&
        mOldInitEntriesMergedWithNewDead ==
            other.mOldInitEntriesMergedWithNewDead &&
        mNewEntriesMergedWithOldNeitherInit ==
            other.mNewEntriesMergedWithOldNeitherInit &&

        mShadowScanSteps == other.mShadowScanSteps &&
        mMetaEntryShadowElisions == other.mMetaEntryShadowElisions &&
        mLiveEntryShadowElisions == other.mLiveEntryShadowElisions &&
        mInitEntryShadowElisions == other.mInitEntryShadowElisions &&
        mDeadEntryShadowElisions == other.mDeadEntryShadowElisions &&

        mOutputIteratorTombstoneElisions ==
            other.mOutputIteratorTombstoneElisions &&
        mOutputIteratorBufferUpdates == other.mOutputIteratorBufferUpdates &&
        mOutputIteratorActualWrites == other.mOutputIteratorActualWrites);
}

void
BucketManagerImpl::incrMergeCounters(MergeCounters const& delta)
{
    std::lock_guard<std::recursive_mutex> lock(mBucketMutex);
    mMergeCounters += delta;
}

bool
BucketManagerImpl::renameBucket(std::string const& src, std::string const& dst)
{
    ZoneScoped;
    if (mApp.getConfig().DISABLE_XDR_FSYNC)
    {
        return rename(src.c_str(), dst.c_str()) == 0;
    }
    else
    {
        return fs::durableRename(src, dst, getBucketDir());
    }
}

std::shared_ptr<Bucket>
BucketManagerImpl::adoptFileAsBucket(std::string const& filename,
                                     uint256 const& hash, size_t nObjects,
                                     size_t nBytes, MergeKey* mergeKey,
                                     std::unique_ptr<BucketIndex const> index)
{
    ZoneScoped;
    releaseAssertOrThrow(mApp.getConfig().MODE_ENABLES_BUCKETLIST);
    std::lock_guard<std::recursive_mutex> lock(mBucketMutex);

    if (mergeKey)
    {
        // If this adoption was a merge, drop any strong reference we were
        // retaining pointing to the std::shared_future it was being produced
        // within (so that we can accurately track references to the bucket via
        // its refcount) and if the adoption succeeds (see below) _retain_ a
        // weak record of the input/output mapping, so we can reconstruct the
        // future if anyone wants to restart the same merge before the bucket
        // expires.
        CLOG_TRACE(Bucket,
                   "BucketManager::adoptFileAsBucket switching merge {} from "
                   "live to finished for output={}",
                   *mergeKey, hexAbbrev(hash));
        mLiveFutures.erase(*mergeKey);
    }

    // Check to see if we have an existing bucket (either in-memory or on-disk)
    std::shared_ptr<Bucket> b = getBucketByHash(hash);
    if (b)
    {
        CLOG_DEBUG(
            Bucket,
            "Deleting bucket file {} that is redundant with existing bucket",
            filename);
        {
            auto timer = LogSlowExecution("Delete redundant bucket");
            std::remove(filename.c_str());

            // race condition: two buckets + indexes were produced in parallel
            // only setIndex if there is no index already.
            maybeSetIndex(b, std::move(index));
        }
    }
    else
    {
        std::string canonicalName = bucketFilename(hash);
        CLOG_DEBUG(Bucket, "Adopting bucket file {} as {}", filename,
                   canonicalName);
        if (!renameBucket(filename, canonicalName))
        {
            std::string err("Failed to rename bucket :");
            err += strerror(errno);
            // it seems there is a race condition with external systems
            // retry after sleeping for a second works around the problem
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (!renameBucket(filename, canonicalName))
            {
                // if rename fails again, surface the original error
                throw std::runtime_error(err);
            }
        }

        b = std::make_shared<Bucket>(canonicalName, hash, std::move(index));
        {
            mSharedBuckets.emplace(hash, b);
            mSharedBucketsSize.set_count(mSharedBuckets.size());
        }
    }
    releaseAssert(b);
    if (mergeKey)
    {
        // Second half of the mergeKey record-keeping, above: if we successfully
        // adopted (no throw), then (weakly) record the preimage of the hash.
        mFinishedMerges.recordMerge(*mergeKey, hash);
    }
    return b;
}

void
BucketManagerImpl::noteEmptyMergeOutput(MergeKey const& mergeKey)
{
    releaseAssertOrThrow(mApp.getConfig().MODE_ENABLES_BUCKETLIST);

    // We _do_ want to remove the mergeKey from mLiveFutures, both so that that
    // map does not grow without bound and more importantly so that we drop the
    // refcount on the input buckets so they get GC'ed from the bucket dir.
    //
    // But: we do _not_ want to store the empty merge in mFinishedMerges,
    // despite it being a theoretically meaningful place to record empty merges,
    // because it'd over-identify multiple individual inputs with the empty
    // output, potentially retaining far too many inputs, as lots of different
    // mergeKeys result in an empty output.
    std::lock_guard<std::recursive_mutex> lock(mBucketMutex);
    CLOG_TRACE(Bucket, "BucketManager::noteEmptyMergeOutput({})", mergeKey);
    mLiveFutures.erase(mergeKey);
}

std::shared_ptr<Bucket>
BucketManagerImpl::getBucketIfExists(uint256 const& hash)
{
    ZoneScoped;
    std::lock_guard<std::recursive_mutex> lock(mBucketMutex);
    auto i = mSharedBuckets.find(hash);
    if (i != mSharedBuckets.end())
    {
        CLOG_TRACE(Bucket,
                   "BucketManager::getBucketIfExists({}) found bucket {}",
                   binToHex(hash), i->second->getFilename());
        return i->second;
    }

    return nullptr;
}

std::shared_ptr<Bucket>
BucketManagerImpl::getBucketByHash(uint256 const& hash)
{
    ZoneScoped;
    std::lock_guard<std::recursive_mutex> lock(mBucketMutex);
    if (isZero(hash))
    {
        return std::make_shared<Bucket>();
    }
    auto i = mSharedBuckets.find(hash);
    if (i != mSharedBuckets.end())
    {
        CLOG_TRACE(Bucket, "BucketManager::getBucketByHash({}) found bucket {}",
                   binToHex(hash), i->second->getFilename());
        return i->second;
    }
    std::string canonicalName = bucketFilename(hash);
    if (fs::exists(canonicalName))
    {
        CLOG_TRACE(Bucket,
                   "BucketManager::getBucketByHash({}) found no bucket, making "
                   "new one",
                   binToHex(hash));

        auto p =
            std::make_shared<Bucket>(canonicalName, hash, /*index=*/nullptr);
        mSharedBuckets.emplace(hash, p);
        mSharedBucketsSize.set_count(mSharedBuckets.size());
        return p;
    }
    return std::shared_ptr<Bucket>();
}

std::shared_future<std::shared_ptr<Bucket>>
BucketManagerImpl::getMergeFuture(MergeKey const& key)
{
    ZoneScoped;
    std::lock_guard<std::recursive_mutex> lock(mBucketMutex);
    MergeCounters mc;
    auto i = mLiveFutures.find(key);
    if (i == mLiveFutures.end())
    {
        // If there's no live (running) future, we might be able to _make_ one
        // for a retained bucket, if we still know its inputs.
        Hash bucketHash;
        if (mFinishedMerges.findMergeFor(key, bucketHash))
        {
            auto bucket = getBucketByHash(bucketHash);
            if (bucket)
            {
                CLOG_TRACE(Bucket,
                           "BucketManager::getMergeFuture returning new future "
                           "for finished merge {} with output={}",
                           key, hexAbbrev(bucketHash));
                std::promise<std::shared_ptr<Bucket>> promise;
                auto future = promise.get_future().share();
                promise.set_value(bucket);
                mc.mFinishedMergeReattachments++;
                incrMergeCounters(mc);
                return future;
            }
        }
        CLOG_TRACE(
            Bucket,
            "BucketManager::getMergeFuture returning empty future for merge {}",
            key);
        return std::shared_future<std::shared_ptr<Bucket>>();
    }
    CLOG_TRACE(
        Bucket,
        "BucketManager::getMergeFuture returning running future for merge {}",
        key);
    mc.mRunningMergeReattachments++;
    incrMergeCounters(mc);
    return i->second;
}

void
BucketManagerImpl::putMergeFuture(
    MergeKey const& key, std::shared_future<std::shared_ptr<Bucket>> wp)
{
    ZoneScoped;
    releaseAssertOrThrow(mApp.getConfig().MODE_ENABLES_BUCKETLIST);
    std::lock_guard<std::recursive_mutex> lock(mBucketMutex);
    CLOG_TRACE(
        Bucket,
        "BucketManager::putMergeFuture storing future for running merge {}",
        key);
    mLiveFutures.emplace(key, wp);
}

#ifdef BUILD_TESTS
void
BucketManagerImpl::clearMergeFuturesForTesting()
{
    std::lock_guard<std::recursive_mutex> lock(mBucketMutex);
    mLiveFutures.clear();
}
#endif

std::set<Hash>
BucketManagerImpl::getBucketListReferencedBuckets() const
{
    ZoneScoped;
    std::set<Hash> referenced;
    if (!mApp.getConfig().MODE_ENABLES_BUCKETLIST)
    {
        return referenced;
    }

    // retain current bucket list
    for (uint32_t i = 0; i < BucketList::kNumLevels; ++i)
    {
        auto const& level = mBucketList->getLevel(i);
        auto rit = referenced.emplace(level.getCurr()->getHash());
        if (rit.second)
        {
            CLOG_TRACE(Bucket, "{} referenced by bucket list",
                       binToHex(*rit.first));
        }
        rit = referenced.emplace(level.getSnap()->getHash());
        if (rit.second)
        {
            CLOG_TRACE(Bucket, "{} referenced by bucket list",
                       binToHex(*rit.first));
        }
        for (auto const& h : level.getNext().getHashes())
        {
            rit = referenced.emplace(hexToBin256(h));
            if (rit.second)
            {
                CLOG_TRACE(Bucket, "{} referenced by bucket list", h);
            }
        }
    }

    return referenced;
}

std::set<Hash>
BucketManagerImpl::getAllReferencedBuckets() const
{
    ZoneScoped;
    auto referenced = getBucketListReferencedBuckets();
    if (!mApp.getConfig().MODE_ENABLES_BUCKETLIST)
    {
        return referenced;
    }

    // retain any bucket referenced by the last closed ledger as recorded in the
    // database (as merges complete, the bucket list drifts from that state)
    auto lclHas = mApp.getLedgerManager().getLastClosedLedgerHAS();
    auto lclBuckets = lclHas.allBuckets();
    for (auto const& h : lclBuckets)
    {
        auto rit = referenced.emplace(hexToBin256(h));
        if (rit.second)
        {
            CLOG_TRACE(Bucket, "{} referenced by LCL", h);
        }
    }

    // retain buckets that are referenced by a state in the publish queue.
    auto pub = mApp.getHistoryManager().getBucketsReferencedByPublishQueue();
    {
        for (auto const& h : pub)
        {
            auto rhash = hexToBin256(h);
            auto rit = referenced.emplace(rhash);
            if (rit.second)
            {
                CLOG_TRACE(Bucket, "{} referenced by publish queue", h);

                // Project referenced bucket `rhash` -- which might be a merge
                // input captured before a merge finished -- through our weak
                // map of merge input/output relationships, to find any outputs
                // we'll want to retain in order to resynthesize the merge in
                // the future, rather than re-run it.
                mFinishedMerges.getOutputsUsingInput(rhash, referenced);
            }
        }
    }
    return referenced;
}

void
BucketManagerImpl::cleanupStaleFiles()
{
    ZoneScoped;
    if (mApp.getConfig().DISABLE_BUCKET_GC)
    {
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(mBucketMutex);
    auto referenced = getAllReferencedBuckets();
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
    ZoneScoped;
    std::lock_guard<std::recursive_mutex> lock(mBucketMutex);
    auto referenced = getAllReferencedBuckets();
    auto blReferenced = getBucketListReferencedBuckets();

    for (auto i = mSharedBuckets.begin(); i != mSharedBuckets.end();)
    {
        // Standard says map iterators other than the one you're erasing
        // remain valid.
        auto j = i;
        ++i;

        // Delete indexes for buckets no longer in bucketlist. There is a race
        // condition on startup where future buckets for a level will be
        // finished and have an index but will not yet be referred to by the
        // bucket level's next pointer. Checking use_count == 1 makes sure no
        // other in-progress structures will add bucket to bucket list after
        // deleting index
        if (j->second->isIndexed() && j->second.use_count() == 1 &&
            blReferenced.find(j->first) == blReferenced.end())
        {
            CLOG_TRACE(Bucket,
                       "BucketManager::forgetUnreferencedBuckets deleting "
                       "index for {}",
                       j->second->getFilename());
            j->second->freeIndex();
        }

        // Only drop buckets if the bucketlist has forgotten them _and_
        // no other in-progress structures (worker threads, shadow lists)
        // have references to them, just us. It's ok to retain a few too
        // many buckets, a little longer than necessary.
        //
        // This conservatism is important because we want to enforce that
        // only one bucket ever exists in memory with a given filename, and
        // that we're the first and last to know about it. Otherwise buckets
        // might race on deleting the underlying file from one another.

        if (referenced.find(j->first) == referenced.end() &&
            j->second.use_count() == 1)
        {
            auto filename = j->second->getFilename();
            CLOG_TRACE(Bucket,
                       "BucketManager::forgetUnreferencedBuckets dropping {}",
                       filename);
            if (!filename.empty() && !mApp.getConfig().DISABLE_BUCKET_GC)
            {
                CLOG_TRACE(Bucket, "removing bucket file: {}", filename);
                std::filesystem::remove(filename);
                auto gzfilename = filename.string() + ".gz";
                std::remove(gzfilename.c_str());
            }

            // Dropping this bucket means we'll no longer be able to
            // resynthesize a std::shared_future pointing directly to it
            // as a short-cut to performing a merge we've already seen.
            // Therefore we should forget it from the weak map we use
            // for that resynthesis.
            for (auto const& forgottenMergeKey :
                 mFinishedMerges.forgetAllMergesProducing(j->first))
            {
                // There should be no futures alive with this output: we
                // switched to storing only weak input/output mappings
                // when any merge producing the bucket completed (in
                // adoptFileAsBucket), and we believe there's only one
                // reference to the bucket anyways -- our own in
                // mSharedBuckets. But there might be a race we missed,
                // so double check & mop up here. Worst case we prevent
                // a slow memory leak at the cost of redoing merges we
                // might have been able to reattach to.
                auto f = mLiveFutures.find(forgottenMergeKey);
                if (f != mLiveFutures.end())
                {
                    CLOG_WARNING(Bucket,
                                 "Unexpected live future for unreferenced "
                                 "bucket: {}",
                                 binToHex(i->first));
                    mLiveFutures.erase(f);
                }
            }

            // All done, delete the bucket from the shared map.
            mSharedBuckets.erase(j);
        }
    }
    mSharedBucketsSize.set_count(mSharedBuckets.size());
}

void
BucketManagerImpl::addBatch(Application& app, uint32_t currLedger,
                            uint32_t currLedgerProtocol,
                            std::vector<LedgerEntry> const& initEntries,
                            std::vector<LedgerEntry> const& liveEntries,
                            std::vector<LedgerKey> const& deadEntries)
{
    ZoneScoped;
    releaseAssertOrThrow(app.getConfig().MODE_ENABLES_BUCKETLIST);
#ifdef BUILD_TESTS
    if (mUseFakeTestValuesForNextClose)
    {
        currLedgerProtocol = mFakeTestProtocolVersion;
    }
#endif
    auto timer = mBucketAddBatch.TimeScope();
    mBucketObjectInsertBatch.Mark(initEntries.size() + liveEntries.size() +
                                  deadEntries.size());
    mBucketList->addBatch(app, currLedger, currLedgerProtocol, initEntries,
                          liveEntries, deadEntries);
}

#ifdef BUILD_TESTS
void
BucketManagerImpl::setNextCloseVersionAndHashForTesting(uint32_t protocolVers,
                                                        uint256 const& hash)
{
    mUseFakeTestValuesForNextClose = true;
    mFakeTestProtocolVersion = protocolVers;
    mFakeTestBucketListHash = hash;
}

std::set<Hash>
BucketManagerImpl::getBucketHashesInBucketDirForTesting() const
{
    std::set<Hash> hashes;
    for (auto f : fs::findfiles(getBucketDir(), isBucketFile))
    {
        hashes.emplace(extractFromFilename(f));
    }
    return hashes;
}
#endif

// updates the given LedgerHeader to reflect the current state of the bucket
// list
void
BucketManagerImpl::snapshotLedger(LedgerHeader& currentHeader)
{
    ZoneScoped;
    Hash hash;
    if (mApp.getConfig().MODE_ENABLES_BUCKETLIST)
    {
        hash = mBucketList->getHash();
    }

    currentHeader.bucketListHash = hash;
#ifdef BUILD_TESTS
    if (mUseFakeTestValuesForNextClose)
    {
        // Copy fake value and disarm for next close.
        currentHeader.bucketListHash = mFakeTestBucketListHash;
        mUseFakeTestValuesForNextClose = false;
    }
#endif
    calculateSkipValues(currentHeader);
}

void
BucketManagerImpl::maybeSetIndex(std::shared_ptr<Bucket> b,
                                 std::unique_ptr<BucketIndex const>&& index)
{
    if (!isShutdown() && index && !b->isIndexed())
    {
        b->setIndex(std::move(index));
    }
}

medida::Timer&
BucketManagerImpl::getBulkLoadTimer(std::string const& label) const
{
    mBucketListDBQueryMeter.Mark();
    auto iter = mBucketListDBBulkTimers.find(label);
    if (iter == mBucketListDBBulkTimers.end())
    {
        auto& metric =
            mApp.getMetrics().NewTimer({"bucketlistDB", "bulk", label});
        iter = mBucketListDBBulkTimers.emplace(label, metric).first;
    }

    return iter->second;
}

medida::Timer&
BucketManagerImpl::getPointLoadTimer(LedgerEntryType t) const
{
    mBucketListDBQueryMeter.Mark();
    auto iter = mBucketListDBPointTimers.find(t);
    if (iter == mBucketListDBPointTimers.end())
    {
        auto const& label = xdr::xdr_traits<LedgerEntryType>::enum_name(t);
        auto& metric =
            mApp.getMetrics().NewTimer({"bucketlistDB", "point", label});
        iter = mBucketListDBPointTimers.emplace(t, metric).first;
    }

    return iter->second;
}

std::shared_ptr<LedgerEntry>
BucketManagerImpl::getLedgerEntry(LedgerKey const& k) const
{
    releaseAssertOrThrow(getConfig().isUsingBucketListDB());
    auto timer = getPointLoadTimer(k.type()).TimeScope();
    return mBucketList->getLedgerEntry(k);
}

std::vector<LedgerEntry>
BucketManagerImpl::loadKeys(
    std::set<LedgerKey, LedgerEntryIdCmp> const& keys) const
{
    releaseAssertOrThrow(getConfig().isUsingBucketListDB());
    auto timer = getBulkLoadTimer("prefetch").TimeScope();
    return mBucketList->loadKeys(keys);
}

std::vector<LedgerEntry>
BucketManagerImpl::loadPoolShareTrustLinesByAccountAndAsset(
    AccountID const& accountID, Asset const& asset) const
{
    releaseAssertOrThrow(getConfig().isUsingBucketListDB());
    auto timer = getBulkLoadTimer("poolshareTrustlines").TimeScope();
    return mBucketList->loadPoolShareTrustLinesByAccountAndAsset(
        accountID, asset, getConfig());
}

std::vector<InflationWinner>
BucketManagerImpl::loadInflationWinners(size_t maxWinners,
                                        int64_t minBalance) const
{
    releaseAssertOrThrow(getConfig().isUsingBucketListDB());
    auto timer = getBulkLoadTimer("inflationWinners").TimeScope();
    return mBucketList->loadInflationWinners(maxWinners, minBalance);
}

medida::Meter&
BucketManagerImpl::getBloomMissMeter() const
{
    return mBucketListDBBloomMisses;
}

medida::Meter&
BucketManagerImpl::getBloomLookupMeter() const
{
    return mBucketListDBBloomLookups;
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
    ZoneScoped;
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
BucketManagerImpl::assumeState(HistoryArchiveState const& has,
                               uint32_t maxProtocolVersion)
{
    ZoneScoped;
    releaseAssertOrThrow(mApp.getConfig().MODE_ENABLES_BUCKETLIST);

    for (uint32_t i = 0; i < BucketList::kNumLevels; ++i)
    {
        auto curr = getBucketByHash(hexToBin256(has.currentBuckets.at(i).curr));
        auto snap = getBucketByHash(hexToBin256(has.currentBuckets.at(i).snap));
        if (!(curr && snap))
        {
            throw std::runtime_error(
                "Missing bucket files while assuming saved BucketList state");
        }

        auto const& nextFuture = has.currentBuckets.at(i).next;
        std::shared_ptr<Bucket> nextBucket = nullptr;
        if (nextFuture.hasOutputHash())
        {
            nextBucket =
                getBucketByHash(hexToBin256(nextFuture.getOutputHash()));
            if (!nextBucket)
            {
                throw std::runtime_error("Missing future bucket files while "
                                         "assuming saved BucketList state");
            }
        }

        // Buckets on the BucketList should always be indexed when BucketListDB
        // enabled
        if (mApp.getConfig().isUsingBucketListDB())
        {
            releaseAssert(curr->isEmpty() || curr->isIndexed());
            releaseAssert(snap->isEmpty() || snap->isIndexed());
            if (nextBucket)
            {
                releaseAssert(nextBucket->isEmpty() || nextBucket->isIndexed());
            }
        }

        mBucketList->getLevel(i).setCurr(curr);
        mBucketList->getLevel(i).setSnap(snap);
        mBucketList->getLevel(i).setNext(nextFuture);
    }

    mBucketList->restartMerges(mApp, maxProtocolVersion, has.currentLedger);
    cleanupStaleFiles();
}

void
BucketManagerImpl::shutdown()
{
    mIsShutdown = true;
}

bool
BucketManagerImpl::isShutdown() const
{
    return mIsShutdown;
}

// Loads a single bucket worth of entries into `map`, deleting dead entries and
// inserting live or init entries. Should be called in a loop over a BL, from
// old to new.
static void
loadEntriesFromBucket(std::shared_ptr<Bucket> b, std::string const& name,
                      std::map<LedgerKey, LedgerEntry>& map)
{
    using namespace std::chrono;
    medida::Timer timer;
    BucketInputIterator in(b);
    timer.Time([&]() {
        while (in)
        {
            BucketEntry const& e = *in;
            if (e.type() == LIVEENTRY || e.type() == INITENTRY)
            {
                map[LedgerEntryKey(e.liveEntry())] = e.liveEntry();
            }
            else
            {
                if (e.type() != DEADENTRY)
                {
                    std::string err = "Malformed bucket: unexpected "
                                      "non-INIT/LIVE/DEAD entry.";
                    CLOG_ERROR(Bucket, "{}", err);
                    throw std::runtime_error(err);
                }
                size_t erased = map.erase(e.deadEntry());
                if (erased != 1)
                {
                    std::string err = fmt::format(
                        FMT_STRING("DEADENTRY does not exist in ledger: {}"),
                        xdr::xdr_to_string(e.deadEntry(), "entry"));
                    CLOG_ERROR(Bucket, "{}", err);
                    throw std::runtime_error(err);
                }
            }
            ++in;
        }
    });
    nanoseconds ns =
        timer.duration_unit() * static_cast<nanoseconds::rep>(timer.max());
    milliseconds ms = duration_cast<milliseconds>(ns);
    size_t bytesPerSec = (b->getSize() * 1000 / (1 + ms.count()));
    CLOG_INFO(Bucket, "Read {}-byte bucket file '{}' in {} ({}/s)",
              b->getSize(), name, ms, formatSize(bytesPerSec));
}

std::map<LedgerKey, LedgerEntry>
BucketManagerImpl::loadCompleteLedgerState(HistoryArchiveState const& has)
{
    std::map<LedgerKey, LedgerEntry> ledgerMap;
    std::vector<std::pair<Hash, std::string>> hashes;
    for (uint32_t i = BucketList::kNumLevels; i > 0; --i)
    {
        HistoryStateBucket const& hsb = has.currentBuckets.at(i - 1);
        hashes.emplace_back(hexToBin256(hsb.snap),
                            fmt::format(FMT_STRING("snap {:d}"), i - 1));
        hashes.emplace_back(hexToBin256(hsb.curr),
                            fmt::format(FMT_STRING("curr {:d}"), i - 1));
    }
    for (auto const& pair : hashes)
    {
        if (isZero(pair.first))
        {
            continue;
        }
        auto b = getBucketByHash(pair.first);
        if (!b)
        {
            throw std::runtime_error(std::string("missing bucket: ") +
                                     binToHex(pair.first));
        }
        loadEntriesFromBucket(b, pair.second, ledgerMap);
    }
    return ledgerMap;
}

std::shared_ptr<Bucket>
BucketManagerImpl::mergeBuckets(HistoryArchiveState const& has)
{
    std::map<LedgerKey, LedgerEntry> ledgerMap = loadCompleteLedgerState(has);
    BucketMetadata meta;
    MergeCounters mc;
    auto& ctx = mApp.getClock().getIOContext();
    meta.ledgerVersion = mApp.getConfig().LEDGER_PROTOCOL_VERSION;
    BucketOutputIterator out(getTmpDir(), /*keepDeadEntries=*/false, meta, mc,
                             ctx, /*doFsync=*/true);
    for (auto const& pair : ledgerMap)
    {
        BucketEntry be;
        be.type(LIVEENTRY);
        be.liveEntry() = pair.second;
        out.put(be);
    }
    return out.getBucket(*this, /*shouldSynchronouslyIndex=*/false);
}

static bool
visitEntriesInBucket(std::shared_ptr<Bucket const> b, std::string const& name,
                     std::optional<int64_t> minLedger,
                     std::function<bool(LedgerEntry const&)> const& filterEntry,
                     std::function<bool(LedgerEntry const&)> const& acceptEntry,
                     UnorderedSet<LedgerKey>& processedEntries)
{
    using namespace std::chrono;
    medida::Timer timer;

    UnorderedMap<LedgerKey, LedgerEntry> bucketEntries;
    bool stopIteration = false;
    timer.Time([&]() {
        for (BucketInputIterator in(b); in; ++in)
        {
            BucketEntry const& e = *in;
            if (e.type() == LIVEENTRY || e.type() == INITENTRY)
            {
                if (minLedger &&
                    e.liveEntry().lastModifiedLedgerSeq < *minLedger)
                {
                    stopIteration = true;
                    continue;
                }
                if (!filterEntry(e.liveEntry()))
                {
                    continue;
                }
                auto key = LedgerEntryKey(e.liveEntry());
                if (processedEntries.find(key) != processedEntries.end())
                {
                    continue;
                }
                bucketEntries[key] = e.liveEntry();
            }
            else
            {
                if (e.type() != DEADENTRY)
                {
                    std::string err = "Malformed bucket: unexpected "
                                      "non-INIT/LIVE/DEAD entry.";
                    CLOG_ERROR(Bucket, "{}", err);
                    throw std::runtime_error(err);
                }
                bucketEntries.erase(e.deadEntry());
                processedEntries.insert(e.deadEntry());
            }
        }
        for (auto const& [key, entry] : bucketEntries)
        {
            processedEntries.insert(key);
            if (!acceptEntry(entry))
            {
                stopIteration = true;
                break;
            }
        }
    });
    nanoseconds ns =
        timer.duration_unit() * static_cast<nanoseconds::rep>(timer.max());
    milliseconds ms = duration_cast<milliseconds>(ns);
    size_t bytesPerSec = (b->getSize() * 1000 / (1 + ms.count()));
    CLOG_INFO(Bucket, "Processed {}-byte bucket file '{}' in {} ({}/s)",
              b->getSize(), name, ms, formatSize(bytesPerSec));
    return !stopIteration;
}

void
BucketManagerImpl::visitLedgerEntries(
    HistoryArchiveState const& has, std::optional<int64_t> minLedger,
    std::function<bool(LedgerEntry const&)> const& filterEntry,
    std::function<bool(LedgerEntry const&)> const& acceptEntry)
{
    UnorderedSet<LedgerKey> deletedEntries;
    std::vector<std::pair<Hash, std::string>> hashes;
    for (uint32_t i = 0; i < BucketList::kNumLevels; ++i)
    {
        HistoryStateBucket const& hsb = has.currentBuckets.at(i);
        hashes.emplace_back(hexToBin256(hsb.curr),
                            fmt::format(FMT_STRING("curr {:d}"), i));
        hashes.emplace_back(hexToBin256(hsb.snap),
                            fmt::format(FMT_STRING("snap {:d}"), i));
    }
    medida::Timer timer;
    timer.Time([&]() {
        for (auto const& pair : hashes)
        {
            if (isZero(pair.first))
            {
                continue;
            }
            auto b = getBucketByHash(pair.first);
            if (!b)
            {
                throw std::runtime_error(std::string("missing bucket: ") +
                                         binToHex(pair.first));
            }
            if (!visitEntriesInBucket(b, pair.second, minLedger, filterEntry,
                                      acceptEntry, deletedEntries))
            {
                break;
            }
        }
    });
    auto ns = timer.duration_unit() *
              static_cast<std::chrono::nanoseconds::rep>(timer.max());
    CLOG_INFO(Bucket, "Total ledger processing time: {}",
              std::chrono::duration_cast<std::chrono::milliseconds>(ns));
}

std::shared_ptr<BasicWork>
BucketManagerImpl::scheduleVerifyReferencedBucketsWork()
{
    std::set<Hash> hashes = getAllReferencedBuckets();
    std::vector<std::shared_ptr<BasicWork>> seq;
    for (auto const& h : hashes)
    {
        if (isZero(h))
        {
            continue;
        }
        auto b = getBucketByHash(h);
        if (!b)
        {
            throw std::runtime_error(fmt::format(
                FMT_STRING("Missing referenced bucket {}"), binToHex(h)));
        }
        seq.emplace_back(std::make_shared<VerifyBucketWork>(
            mApp, b->getFilename().string(), b->getHash(), nullptr));
    }
    return mApp.getWorkScheduler().scheduleWork<WorkSequence>(
        "verify-referenced-buckets", seq);
}

Config const&
BucketManagerImpl::getConfig() const
{
    return mApp.getConfig();
}
}
