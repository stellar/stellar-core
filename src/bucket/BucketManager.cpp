// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketManager.h"
#include "bucket/BucketInputIterator.h"
#include "bucket/BucketManager.h"
#include "bucket/BucketOutputIterator.h"
#include "bucket/BucketSnapshotManager.h"
#include "bucket/BucketUtils.h"
#include "bucket/HotArchiveBucket.h"
#include "bucket/HotArchiveBucketList.h"
#include "bucket/LiveBucket.h"
#include "bucket/LiveBucketList.h"
#include "crypto/BLAKE2.h"
#include "crypto/Hex.h"
#include "history/HistoryManager.h"
#include "historywork/VerifyBucketWork.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTypeUtils.h"
#include "ledger/NetworkConfig.h"
#include "main/Application.h"
#include "main/Config.h"
#include "util/Fs.h"
#include "util/GlobalChecks.h"
#include "util/LogSlowExecution.h"
#include "util/Logging.h"
#include "util/ProtocolVersion.h"
#include "util/TmpDir.h"
#include "util/types.h"
#include "xdr/Stellar-ledger.h"
#include <filesystem>
#include <fmt/chrono.h>
#include <fmt/format.h>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <thread>

#include "history/FileTransferInfo.h"
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
    auto bucketManagerPtr =
        std::unique_ptr<BucketManager>(new BucketManager(app));
    bucketManagerPtr->initialize();
    return bucketManagerPtr;
}

void
BucketManager::initialize()
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    std::string d = mConfig.BUCKET_DIR_PATH;

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

    if (mConfig.MODE_ENABLES_BUCKETLIST)
    {
        mLiveBucketList = std::make_unique<LiveBucketList>();
        mHotArchiveBucketList = std::make_unique<HotArchiveBucketList>();
        mSnapshotManager = std::make_unique<BucketSnapshotManager>(
            mApp,
            std::make_unique<BucketListSnapshot<LiveBucket>>(*mLiveBucketList,
                                                             LedgerHeader()),
            std::make_unique<BucketListSnapshot<HotArchiveBucket>>(
                *mHotArchiveBucketList, LedgerHeader()),
            mConfig.QUERY_SNAPSHOT_LEDGERS);
    }

    // Create persistent publish directories
    // Note: HISTORY_FILE_TYPE_BUCKET is already tracked by BucketList in
    // BUCKET_DIR_PATH, HISTORY_FILE_TYPE_SCP is persisted to the database
    // so create the remaining ledger header, transactions and results
    // directories
    createPublishDir(FileType::HISTORY_FILE_TYPE_LEDGER, mConfig);
    createPublishDir(FileType::HISTORY_FILE_TYPE_TRANSACTIONS, mConfig);
    createPublishDir(FileType::HISTORY_FILE_TYPE_RESULTS, mConfig);
    HistoryManager::createPublishQueueDir(mConfig);
}

void
BucketManager::dropAll()
{
    ZoneScoped;
    deleteEntireBucketDir();
    initialize();
}

TmpDirManager&
BucketManager::getTmpDirManager()
{
    return *mTmpDirManager;
}

BucketManager::BucketManager(Application& app)
    : mApp(app)
    , mLiveBucketList(nullptr)
    , mHotArchiveBucketList(nullptr)
    , mSnapshotManager(nullptr)
    , mTmpDirManager(nullptr)
    , mWorkDir(nullptr)
    , mLockedBucketDir(nullptr)
    , mBucketLiveObjectInsertBatch(app.getMetrics().NewMeter(
          {"bucket", "batch", "objectsadded"}, "object"))
    , mBucketArchiveObjectInsertBatch(app.getMetrics().NewMeter(
          {"bucket", "batch-archive", "objectsadded"}, "object"))
    , mBucketAddLiveBatch(
          app.getMetrics().NewTimer({"bucket", "batch", "addtime"}))
    , mBucketAddArchiveBatch(
          app.getMetrics().NewTimer({"bucket", "batch-archive", "addtime"}))
    , mBucketSnapMerge(app.getMetrics().NewTimer({"bucket", "snap", "merge"}))
    , mSharedBucketsSize(
          app.getMetrics().NewCounter({"bucket", "memory", "shared"}))
    , mLiveBucketListSizeCounter(
          app.getMetrics().NewCounter({"bucketlist", "size", "bytes"}))
    , mArchiveBucketListSizeCounter(
          app.getMetrics().NewCounter({"bucketlist-archive", "size", "bytes"}))
    , mBucketListEvictionCounters(app)
    , mEvictionStatistics(std::make_shared<EvictionStatistics>())
    , mConfig(app.getConfig())
{
    for (uint32_t t =
             static_cast<uint32_t>(LedgerEntryTypeAndDurability::ACCOUNT);
         t < static_cast<uint32_t>(LedgerEntryTypeAndDurability::NUM_TYPES);
         ++t)
    {
        auto type = static_cast<LedgerEntryTypeAndDurability>(t);
        auto typeString = toString(type);
        mBucketListEntryCountCounters.emplace(
            type, app.getMetrics().NewCounter(
                      {"bucketlist", "entryCounts", typeString}));
        mBucketListEntrySizeCounters.emplace(
            type, app.getMetrics().NewCounter(
                      {"bucketlist", "entrySizes", typeString}));
    }
}

const std::string BucketManager::kLockFilename = "stellar-core.lock";

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

void
BucketManager::updateSharedBucketSize()
{
    mSharedBucketsSize.set_count(mSharedHotArchiveBuckets.size() +
                                 mSharedLiveBuckets.size());
}

std::string
BucketManager::bucketFilename(std::string const& bucketHexHash)
{
    std::string basename = bucketBasename(bucketHexHash);
    return getBucketDir() + "/" + basename;
}

std::string
BucketManager::bucketFilename(Hash const& hash)
{
    return bucketFilename(binToHex(hash));
}

std::string
BucketManager::bucketIndexFilename(Hash const& hash) const
{
    auto hashStr = binToHex(hash);
    auto basename = "bucket-" + hashStr + ".index";
    return getBucketDir() + "/" + basename;
}

std::string const&
BucketManager::getTmpDir()
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
BucketManager::getBucketDir() const
{
    return *(mLockedBucketDir);
}

BucketManager::~BucketManager()
{

    deleteTmpDirAndUnlockBucketDir();
}

void
BucketManager::deleteEntireBucketDir()
{
    ZoneScoped;
    std::string d = mConfig.BUCKET_DIR_PATH;
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
BucketManager::deleteTmpDirAndUnlockBucketDir()
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
        std::string d = mConfig.BUCKET_DIR_PATH;
        std::string lock = d + "/" + kLockFilename;
        releaseAssert(fs::exists(lock));
        fs::unlockFile(lock);
        mLockedBucketDir.reset();
    }
}

LiveBucketList&
BucketManager::getLiveBucketList()
{
    releaseAssertOrThrow(mConfig.MODE_ENABLES_BUCKETLIST);
    return *mLiveBucketList;
}

HotArchiveBucketList&
BucketManager::getHotArchiveBucketList()
{
    releaseAssertOrThrow(mConfig.MODE_ENABLES_BUCKETLIST);
    return *mHotArchiveBucketList;
}

BucketSnapshotManager&
BucketManager::getBucketSnapshotManager() const
{
    releaseAssert(mSnapshotManager);
    return *mSnapshotManager;
}

medida::Timer&
BucketManager::getMergeTimer()
{
    return mBucketSnapMerge;
}

template <class BucketT>
medida::Meter&
BucketManager::getBloomMissMeter() const
{
    BUCKET_TYPE_ASSERT(BucketT);
    return mApp.getMetrics().NewMeter(
        {BucketT::METRIC_STRING, "bloom", "misses"}, "bloom");
}

template <class BucketT>
medida::Meter&
BucketManager::getBloomLookupMeter() const
{
    BUCKET_TYPE_ASSERT(BucketT);
    return mApp.getMetrics().NewMeter(
        {BucketT::METRIC_STRING, "bloom", "lookups"}, "bloom");
}

MergeCounters
BucketManager::readMergeCounters()
{
    std::lock_guard<std::recursive_mutex> lock(mBucketMutex);
    return mMergeCounters;
}

void
BucketManager::incrMergeCounters(MergeCounters const& delta)
{
    std::lock_guard<std::recursive_mutex> lock(mBucketMutex);
    mMergeCounters += delta;
}

bool
BucketManager::renameBucketDirFile(std::filesystem::path const& src,
                                   std::filesystem::path const& dst)
{
    ZoneScoped;
    if (mConfig.DISABLE_XDR_FSYNC)
    {
        return rename(src.string().c_str(), dst.string().c_str()) == 0;
    }
    else
    {
        return fs::durableRename(src.string(), dst.string(), getBucketDir());
    }
}

template <>
std::shared_ptr<LiveBucket>
BucketManager::adoptFileAsBucket(
    std::string const& filename, uint256 const& hash, MergeKey* mergeKey,
    std::unique_ptr<LiveBucket::IndexT const> index)
{
    return adoptFileAsBucketInternal(filename, hash, mergeKey, std::move(index),
                                     mSharedLiveBuckets, mLiveBucketFutures);
}

template <>
std::shared_ptr<HotArchiveBucket>
BucketManager::adoptFileAsBucket(
    std::string const& filename, uint256 const& hash, MergeKey* mergeKey,
    std::unique_ptr<HotArchiveBucket::IndexT const> index)
{
    return adoptFileAsBucketInternal(filename, hash, mergeKey, std::move(index),
                                     mSharedHotArchiveBuckets,
                                     mHotArchiveBucketFutures);
}

template <typename BucketT>
std::shared_ptr<BucketT>
BucketManager::adoptFileAsBucketInternal(
    std::string const& filename, uint256 const& hash, MergeKey* mergeKey,
    std::unique_ptr<typename BucketT::IndexT const> index,
    BucketMapT<BucketT>& bucketMap, FutureMapT<BucketT>& futureMap)
{
    BUCKET_TYPE_ASSERT(BucketT);
    ZoneScoped;
    releaseAssertOrThrow(mConfig.MODE_ENABLES_BUCKETLIST);
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
        CLOG_TRACE(
            Bucket,
            "BucketManager::adoptFileAsLiveBucket switching merge {} from "
            "live to finished for output={}",
            *mergeKey, hexAbbrev(hash));
        futureMap.erase(*mergeKey);
    }

    // Check to see if we have an existing bucket (either in-memory or on-disk)
    std::shared_ptr<BucketT> b = getBucketByHashInternal(hash, bucketMap);
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
        if (!renameBucketDirFile(filename, canonicalName))
        {
            std::string err("Failed to rename bucket :");
            err += strerror(errno);
            // it seems there is a race condition with external systems
            // retry after sleeping for a second works around the problem
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (!renameBucketDirFile(filename, canonicalName))
            {
                // if rename fails again, surface the original error
                throw std::runtime_error(err);
            }
        }

        b = std::make_shared<BucketT>(canonicalName, hash, std::move(index));
        {
            bucketMap.emplace(hash, b);
            updateSharedBucketSize();
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

template <>
void
BucketManager::noteEmptyMergeOutput<LiveBucket>(MergeKey const& mergeKey)
{
    noteEmptyMergeOutputInternal(mergeKey, mLiveBucketFutures);
}

template <>
void
BucketManager::noteEmptyMergeOutput<HotArchiveBucket>(MergeKey const& mergeKey)
{
    noteEmptyMergeOutputInternal(mergeKey, mHotArchiveBucketFutures);
}

template <class BucketT>
void
BucketManager::noteEmptyMergeOutputInternal(MergeKey const& mergeKey,
                                            FutureMapT<BucketT>& futureMap)
{
    BUCKET_TYPE_ASSERT(BucketT);
    releaseAssertOrThrow(mConfig.MODE_ENABLES_BUCKETLIST);

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
    futureMap.erase(mergeKey);
}

template <>
std::shared_ptr<LiveBucket>
BucketManager::getBucketIfExists(uint256 const& hash)
{
    return getBucketIfExistsInternal(hash, mSharedLiveBuckets);
}

template <>
std::shared_ptr<HotArchiveBucket>
BucketManager::getBucketIfExists(uint256 const& hash)
{
    return getBucketIfExistsInternal(hash, mSharedHotArchiveBuckets);
}

template <class BucketT>
std::shared_ptr<BucketT>
BucketManager::getBucketIfExistsInternal(
    uint256 const& hash, BucketMapT<BucketT> const& bucketMap) const
{
    BUCKET_TYPE_ASSERT(BucketT);
    ZoneScoped;
    std::lock_guard<std::recursive_mutex> lock(mBucketMutex);
    auto i = bucketMap.find(hash);
    if (i != bucketMap.end())
    {
        CLOG_TRACE(Bucket,
                   "BucketManager::getBucketIfExists({}) found bucket {}",
                   binToHex(hash), i->second->getFilename());
        return i->second;
    }

    return nullptr;
}

template <>
std::shared_ptr<LiveBucket>
BucketManager::getBucketByHash(uint256 const& hash)
{
    return getBucketByHashInternal(hash, mSharedLiveBuckets);
}

template <>
std::shared_ptr<HotArchiveBucket>
BucketManager::getBucketByHash(uint256 const& hash)
{
    return getBucketByHashInternal(hash, mSharedHotArchiveBuckets);
}

template <class BucketT>
std::shared_ptr<BucketT>
BucketManager::getBucketByHashInternal(uint256 const& hash,
                                       BucketMapT<BucketT>& bucketMap)
{
    BUCKET_TYPE_ASSERT(BucketT);
    ZoneScoped;
    std::lock_guard<std::recursive_mutex> lock(mBucketMutex);
    if (isZero(hash))
    {
        return std::make_shared<BucketT>();
    }
    auto i = bucketMap.find(hash);
    if (i != bucketMap.end())
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
            std::make_shared<BucketT>(canonicalName, hash, /*index=*/nullptr);
        bucketMap.emplace(hash, p);
        updateSharedBucketSize();
        return p;
    }
    return std::shared_ptr<BucketT>();
}

template <>
std::shared_future<std::shared_ptr<LiveBucket>>
BucketManager::getMergeFuture(MergeKey const& key)
{
    return getMergeFutureInternal(key, mLiveBucketFutures);
}

template <>
std::shared_future<std::shared_ptr<HotArchiveBucket>>
BucketManager::getMergeFuture(MergeKey const& key)
{
    return getMergeFutureInternal(key, mHotArchiveBucketFutures);
}

template <class BucketT>
std::shared_future<std::shared_ptr<BucketT>>
BucketManager::getMergeFutureInternal(MergeKey const& key,
                                      FutureMapT<BucketT>& futureMap)
{
    BUCKET_TYPE_ASSERT(BucketT);
    ZoneScoped;
    std::lock_guard<std::recursive_mutex> lock(mBucketMutex);
    MergeCounters mc;
    auto i = futureMap.find(key);
    if (i == futureMap.end())
    {
        // If there's no live (running) future, we might be able to _make_ one
        // for a retained bucket, if we still know its inputs.
        Hash bucketHash;
        if (mFinishedMerges.findMergeFor(key, bucketHash))
        {
            auto bucket = BucketManager::getBucketByHash<BucketT>(bucketHash);
            if (bucket)
            {
                CLOG_TRACE(Bucket,
                           "BucketManager::getMergeFuture returning new future "
                           "for finished merge {} with output={}",
                           key, hexAbbrev(bucketHash));
                std::promise<std::shared_ptr<BucketT>> promise;
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
        return std::shared_future<std::shared_ptr<BucketT>>();
    }
    CLOG_TRACE(
        Bucket,
        "BucketManager::getMergeFuture returning running future for merge {}",
        key);
    mc.mRunningMergeReattachments++;
    incrMergeCounters(mc);
    return i->second;
}

template <>
void
BucketManager::putMergeFuture(
    MergeKey const& key, std::shared_future<std::shared_ptr<LiveBucket>> future)
{
    putMergeFutureInternal(key, future, mLiveBucketFutures);
}

template <>
void
BucketManager::putMergeFuture(
    MergeKey const& key,
    std::shared_future<std::shared_ptr<HotArchiveBucket>> future)
{
    putMergeFutureInternal(key, future, mHotArchiveBucketFutures);
}

template <class BucketT>
void
BucketManager::putMergeFutureInternal(
    MergeKey const& key, std::shared_future<std::shared_ptr<BucketT>> future,
    FutureMapT<BucketT>& futureMap)
{
    BUCKET_TYPE_ASSERT(BucketT);
    ZoneScoped;
    releaseAssertOrThrow(mConfig.MODE_ENABLES_BUCKETLIST);
    std::lock_guard<std::recursive_mutex> lock(mBucketMutex);
    CLOG_TRACE(
        Bucket,
        "BucketManager::putMergeFuture storing future for running merge {}",
        key);
    futureMap.emplace(key, future);
}

#ifdef BUILD_TESTS
void
BucketManager::clearMergeFuturesForTesting()
{
    std::lock_guard<std::recursive_mutex> lock(mBucketMutex);
    mLiveBucketFutures.clear();
    mHotArchiveBucketFutures.clear();
}
#endif

std::set<Hash>
BucketManager::getBucketListReferencedBuckets() const
{
    ZoneScoped;
    std::set<Hash> referenced;
    if (!mConfig.MODE_ENABLES_BUCKETLIST)
    {
        return referenced;
    }

    auto processBucketList = [&](auto const& bl, uint32_t levels) {
        // retain current bucket list
        for (uint32_t i = 0; i < levels; ++i)
        {
            auto const& level = bl->getLevel(i);
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
    };

    processBucketList(mLiveBucketList, LiveBucketList::kNumLevels);
    processBucketList(mHotArchiveBucketList,
                      BucketListBase<HotArchiveBucket>::kNumLevels);

    return referenced;
}

std::set<Hash>
BucketManager::getAllReferencedBuckets(HistoryArchiveState const& has) const
{
    ZoneScoped;
    auto referenced = getBucketListReferencedBuckets();
    if (!mConfig.MODE_ENABLES_BUCKETLIST)
    {
        return referenced;
    }

    // retain any bucket referenced by the last closed ledger as recorded in the
    // database (as merges complete, the bucket list drifts from that state)
    auto lclBuckets = has.allBuckets();
    for (auto const& h : lclBuckets)
    {
        auto rit = referenced.emplace(hexToBin256(h));
        if (rit.second)
        {
            CLOG_TRACE(Bucket, "{} referenced by LCL", h);
        }
    }

    // retain buckets that are referenced by a state in the publish queue.
    for (auto const& h :
         HistoryManager::getBucketsReferencedByPublishQueue(mApp.getConfig()))
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
    return referenced;
}

void
BucketManager::cleanupStaleFiles(HistoryArchiveState const& has)
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    if (mConfig.DISABLE_BUCKET_GC)
    {
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(mBucketMutex);
    auto referenced = getAllReferencedBuckets(has);
    std::transform(std::begin(mSharedLiveBuckets), std::end(mSharedLiveBuckets),
                   std::inserter(referenced, std::end(referenced)),
                   [](std::pair<Hash, std::shared_ptr<LiveBucket>> const& p) {
                       return p.first;
                   });

    std::transform(
        std::begin(mSharedHotArchiveBuckets),
        std::end(mSharedHotArchiveBuckets),
        std::inserter(referenced, std::end(referenced)),
        [](std::pair<Hash, std::shared_ptr<HotArchiveBucket>> const& p) {
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

            // GC index as well
            auto indexFilename = bucketIndexFilename(hash);
            std::remove(indexFilename.c_str());
        }
    }
}

void
BucketManager::forgetUnreferencedBuckets(HistoryArchiveState const& has)
{
    ZoneScoped;
    std::lock_guard<std::recursive_mutex> lock(mBucketMutex);
    auto referenced = getAllReferencedBuckets(has);
    auto blReferenced = getBucketListReferencedBuckets();

    auto bucketMapLoop = [&](auto& bucketMap, auto& futureMap) {
        for (auto i = bucketMap.begin(); i != bucketMap.end();)
        {
            // Standard says map iterators other than the one you're erasing
            // remain valid.
            auto j = i;
            ++i;

            // Delete indexes for buckets no longer in bucketlist. There is a
            // race condition on startup where future buckets for a level will
            // be finished and have an index but will not yet be referred to by
            // the bucket level's next pointer. Checking use_count == 1 makes
            // sure no other in-progress structures will add bucket to bucket
            // list after deleting index
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
                CLOG_TRACE(
                    Bucket,
                    "BucketManager::forgetUnreferencedBuckets dropping {}",
                    filename);
                if (!filename.empty() && !mConfig.DISABLE_BUCKET_GC)
                {
                    CLOG_TRACE(Bucket, "removing bucket file: {}", filename);
                    std::filesystem::remove(filename);
                    auto gzfilename = filename.string() + ".gz";
                    std::remove(gzfilename.c_str());
                    auto indexFilename =
                        bucketIndexFilename(j->second->getHash());
                    std::remove(indexFilename.c_str());
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
                    // adoptFileAsLiveBucket), and we believe there's only one
                    // reference to the bucket anyways -- our own in
                    // mSharedBuckets. But there might be a race we missed,
                    // so double check & mop up here. Worst case we prevent
                    // a slow memory leak at the cost of redoing merges we
                    // might have been able to reattach to.
                    auto f = futureMap.find(forgottenMergeKey);
                    if (f != futureMap.end())
                    {
                        CLOG_WARNING(Bucket,
                                     "Unexpected live future for unreferenced "
                                     "bucket: {}",
                                     binToHex(i->first));
                        futureMap.erase(f);
                    }
                }

                // All done, delete the bucket from the shared map.
                bucketMap.erase(j);
            }
        }
    };

    bucketMapLoop(mSharedLiveBuckets, mLiveBucketFutures);
    bucketMapLoop(mSharedHotArchiveBuckets, mHotArchiveBucketFutures);
    updateSharedBucketSize();
}

void
BucketManager::addLiveBatch(Application& app, LedgerHeader header,
                            std::vector<LedgerEntry> const& initEntries,
                            std::vector<LedgerEntry> const& liveEntries,
                            std::vector<LedgerKey> const& deadEntries)
{
    ZoneScoped;
    releaseAssertOrThrow(app.getConfig().MODE_ENABLES_BUCKETLIST);
#ifdef BUILD_TESTS
    if (mUseFakeTestValuesForNextClose)
    {
        header.ledgerVersion = mFakeTestProtocolVersion;
    }
#endif
    auto timer = mBucketAddLiveBatch.TimeScope();
    mBucketLiveObjectInsertBatch.Mark(initEntries.size() + liveEntries.size() +
                                      deadEntries.size());
    mLiveBucketList->addBatch(app, header.ledgerSeq, header.ledgerVersion,
                              initEntries, liveEntries, deadEntries);
    mLiveBucketListSizeCounter.set_count(mLiveBucketList->getSize());
    reportBucketEntryCountMetrics();
}

void
BucketManager::addHotArchiveBatch(
    Application& app, LedgerHeader header,
    std::vector<LedgerEntry> const& archivedEntries,
    std::vector<LedgerKey> const& restoredEntries,
    std::vector<LedgerKey> const& deletedEntries)
{
    ZoneScoped;
    releaseAssertOrThrow(app.getConfig().MODE_ENABLES_BUCKETLIST);
    releaseAssertOrThrow(protocolVersionStartsFrom(
        header.ledgerVersion,
        HotArchiveBucket::FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION));
#ifdef BUILD_TESTS
    if (mUseFakeTestValuesForNextClose)
    {
        header.ledgerVersion = mFakeTestProtocolVersion;
    }
#endif
    auto timer = mBucketAddArchiveBatch.TimeScope();
    mBucketArchiveObjectInsertBatch.Mark(archivedEntries.size() +
                                         restoredEntries.size() +
                                         deletedEntries.size());

    // Hot archive should never modify an existing entry, so there are never
    // live entries
    mHotArchiveBucketList->addBatch(app, header.ledgerSeq, header.ledgerVersion,
                                    archivedEntries, restoredEntries,
                                    deletedEntries);
    mArchiveBucketListSizeCounter.set_count(mHotArchiveBucketList->getSize());
}

#ifdef BUILD_TESTS
void
BucketManager::setNextCloseVersionAndHashForTesting(uint32_t protocolVers,
                                                    uint256 const& hash)
{
    mUseFakeTestValuesForNextClose = true;
    mFakeTestProtocolVersion = protocolVers;
    mFakeTestBucketListHash = hash;
}

std::set<Hash>
BucketManager::getBucketHashesInBucketDirForTesting() const
{
    std::set<Hash> hashes;
    for (auto f : fs::findfiles(getBucketDir(), isBucketFile))
    {
        hashes.emplace(extractFromFilename(f));
    }
    return hashes;
}

medida::Counter&
BucketManager::getEntriesEvictedCounter() const
{
    return mBucketListEvictionCounters.entriesEvicted;
}
#endif

// updates the given LedgerHeader to reflect the current state of the bucket
// list
void
BucketManager::snapshotLedger(LedgerHeader& currentHeader)
{
    ZoneScoped;
    Hash hash;
    if (mConfig.MODE_ENABLES_BUCKETLIST)
    {
        if (protocolVersionStartsFrom(
                currentHeader.ledgerVersion,
                HotArchiveBucket::
                    FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION))
        {
            // TODO: Hash Archive Bucket
            // Dependency: HAS supports Hot Archive BucketList

            hash = mLiveBucketList->getHash();
        }
        else
        {
            hash = mLiveBucketList->getHash();
        }
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

template <class BucketT>
void
BucketManager::maybeSetIndex(
    std::shared_ptr<BucketT> b,
    std::unique_ptr<typename BucketT::IndexT const>&& index)
{
    ZoneScoped;

    if (!isShutdown() && index && !b->isIndexed())
    {
        b->setIndex(std::move(index));
    }
}

void
BucketManager::startBackgroundEvictionScan(uint32_t ledgerSeq,
                                           uint32_t ledgerVers,
                                           SorobanNetworkConfig const& cfg)
{
    releaseAssert(mSnapshotManager);
    releaseAssert(!mEvictionFuture.valid());
    releaseAssert(mEvictionStatistics);

    auto searchableBL =
        mSnapshotManager->copySearchableLiveBucketListSnapshot();
    auto const& sas = cfg.stateArchivalSettings();

    using task_t = std::packaged_task<EvictionResultCandidates()>;
    // MSVC gotcha: searchableBL has to be shared_ptr because MSVC wants to
    // copy this lambda, otherwise we could use unique_ptr.
    auto task = std::make_shared<task_t>(
        [bl = std::move(searchableBL), iter = cfg.evictionIterator(), ledgerSeq,
         ledgerVers, sas, &counters = mBucketListEvictionCounters,
         stats = mEvictionStatistics] {
            return bl->scanForEviction(ledgerSeq, counters, iter, stats, sas,
                                       ledgerVers);
        });

    mEvictionFuture = task->get_future();
    mApp.postOnEvictionBackgroundThread(
        bind(&task_t::operator(), task),
        "SearchableLiveBucketListSnapshot: eviction scan");
}

EvictedStateVectors
BucketManager::resolveBackgroundEvictionScan(
    AbstractLedgerTxn& ltx, uint32_t ledgerSeq,
    LedgerKeySet const& modifiedKeys, uint32_t ledgerVers,
    SorobanNetworkConfig const& networkConfig)
{
    ZoneScoped;
    releaseAssert(mEvictionStatistics);

    if (!mEvictionFuture.valid())
    {
        startBackgroundEvictionScan(ledgerSeq, ledgerVers, networkConfig);
    }

    auto evictionCandidates = mEvictionFuture.get();

    // If eviction related settings changed during the ledger, we have to
    // restart the scan
    if (!evictionCandidates.isValid(ledgerSeq,
                                    networkConfig.stateArchivalSettings()))
    {
        startBackgroundEvictionScan(ledgerSeq, ledgerVers, networkConfig);
        evictionCandidates = mEvictionFuture.get();
    }

    auto& eligibleEntries = evictionCandidates.eligibleEntries;

    for (auto iter = eligibleEntries.begin(); iter != eligibleEntries.end();)
    {
        // If the TTL has not been modified this ledger, we can evict the entry
        if (modifiedKeys.find(getTTLKey(iter->entry)) == modifiedKeys.end())
        {
            ++iter;
        }
        else
        {
            iter = eligibleEntries.erase(iter);
        }
    }

    auto remainingEntriesToEvict =
        networkConfig.stateArchivalSettings().maxEntriesToArchive;
    auto entryToEvictIter = eligibleEntries.begin();
    auto newEvictionIterator = evictionCandidates.endOfRegionIterator;

    // Return vectors include both evicted entry and associated TTL
    std::vector<LedgerKey> deletedKeys;
    std::vector<LedgerEntry> archivedEntries;

    // Only actually evict up to maxEntriesToArchive of the eligible entries
    while (remainingEntriesToEvict > 0 &&
           entryToEvictIter != eligibleEntries.end())
    {
        ltx.erase(LedgerEntryKey(entryToEvictIter->entry));
        ltx.erase(getTTLKey(entryToEvictIter->entry));
        --remainingEntriesToEvict;

        if (isTemporaryEntry(entryToEvictIter->entry.data))
        {
            deletedKeys.emplace_back(LedgerEntryKey(entryToEvictIter->entry));
        }
        else
        {
            archivedEntries.emplace_back(entryToEvictIter->entry);
        }

        // Delete TTL for both types
        deletedKeys.emplace_back(getTTLKey(entryToEvictIter->entry));

        auto age = ledgerSeq - entryToEvictIter->liveUntilLedger;
        mEvictionStatistics->recordEvictedEntry(age);
        mBucketListEvictionCounters.entriesEvicted.inc();

        newEvictionIterator = entryToEvictIter->iter;
        entryToEvictIter = eligibleEntries.erase(entryToEvictIter);
    }

    // If remainingEntriesToEvict == 0, that means we could not evict the entire
    // scan region, so the new eviction iterator should be after the last entry
    // evicted. Otherwise, eviction iterator should be at the end of the scan
    // region
    if (remainingEntriesToEvict != 0)
    {
        newEvictionIterator = evictionCandidates.endOfRegionIterator;
    }

    networkConfig.updateEvictionIterator(ltx, newEvictionIterator);
    return EvictedStateVectors{deletedKeys, archivedEntries};
}

void
BucketManager::calculateSkipValues(LedgerHeader& currentHeader)
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
BucketManager::checkForMissingBucketsFiles(HistoryArchiveState const& has)
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
BucketManager::assumeState(HistoryArchiveState const& has,
                           uint32_t maxProtocolVersion, bool restartMerges)
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    releaseAssertOrThrow(mConfig.MODE_ENABLES_BUCKETLIST);

    // TODO: Assume archival bucket state
    // Dependency: HAS supports Hot Archive BucketList
    for (uint32_t i = 0; i < LiveBucketList::kNumLevels; ++i)
    {
        auto curr = getBucketByHashInternal(
            hexToBin256(has.currentBuckets.at(i).curr), mSharedLiveBuckets);
        auto snap = getBucketByHashInternal(
            hexToBin256(has.currentBuckets.at(i).snap), mSharedLiveBuckets);
        if (!(curr && snap))
        {
            throw std::runtime_error("Missing bucket files while assuming "
                                     "saved live BucketList state");
        }

        auto const& nextFuture = has.currentBuckets.at(i).next;
        std::shared_ptr<LiveBucket> nextBucket = nullptr;
        if (nextFuture.hasOutputHash())
        {
            nextBucket = getBucketByHashInternal(
                hexToBin256(nextFuture.getOutputHash()), mSharedLiveBuckets);
            if (!nextBucket)
            {
                throw std::runtime_error(
                    "Missing future bucket files while "
                    "assuming saved live BucketList state");
            }
        }

        // Buckets on the BucketList should always be indexed
        releaseAssert(curr->isEmpty() || curr->isIndexed());
        releaseAssert(snap->isEmpty() || snap->isIndexed());
        if (nextBucket)
        {
            releaseAssert(nextBucket->isEmpty() || nextBucket->isIndexed());
        }

        mLiveBucketList->getLevel(i).setCurr(curr);
        mLiveBucketList->getLevel(i).setSnap(snap);
        mLiveBucketList->getLevel(i).setNext(nextFuture);
    }

    if (restartMerges)
    {
        mLiveBucketList->restartMerges(mApp, maxProtocolVersion,
                                       has.currentLedger);
    }
    cleanupStaleFiles(has);
}

void
BucketManager::shutdown()
{
    mIsShutdown = true;
}

bool
BucketManager::isShutdown() const
{
    return mIsShutdown;
}

// Loads a single bucket worth of entries into `map`, deleting dead entries and
// inserting live or init entries. Should be called in a loop over a BL, from
// old to new.
static void
loadEntriesFromBucket(std::shared_ptr<LiveBucket> b, std::string const& name,
                      std::map<LedgerKey, LedgerEntry>& map)
{
    ZoneScoped;

    using namespace std::chrono;
    medida::Timer timer;
    LiveBucketInputIterator in(b);
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
BucketManager::loadCompleteLedgerState(HistoryArchiveState const& has)
{
    ZoneScoped;

    std::map<LedgerKey, LedgerEntry> ledgerMap;
    std::vector<std::pair<Hash, std::string>> hashes;
    for (uint32_t i = LiveBucketList::kNumLevels; i > 0; --i)
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
        auto b = getBucketByHashInternal(pair.first, mSharedLiveBuckets);
        if (!b)
        {
            throw std::runtime_error(std::string("missing bucket: ") +
                                     binToHex(pair.first));
        }
        loadEntriesFromBucket(b, pair.second, ledgerMap);
    }
    return ledgerMap;
}

std::shared_ptr<LiveBucket>
BucketManager::mergeBuckets(HistoryArchiveState const& has)
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    std::map<LedgerKey, LedgerEntry> ledgerMap = loadCompleteLedgerState(has);
    BucketMetadata meta;
    MergeCounters mc;
    auto& ctx = mApp.getClock().getIOContext();
    meta.ledgerVersion = mConfig.LEDGER_PROTOCOL_VERSION;
    LiveBucketOutputIterator out(getTmpDir(), /*keepTombstoneEntries=*/false,
                                 meta, mc, ctx, /*doFsync=*/true);
    for (auto const& pair : ledgerMap)
    {
        BucketEntry be;
        be.type(LIVEENTRY);
        be.liveEntry() = pair.second;
        out.put(be);
    }
    return out.getBucket(*this);
}

static bool
visitLiveEntriesInBucket(
    std::shared_ptr<LiveBucket const> b, std::string const& name,
    std::optional<int64_t> minLedger,
    std::function<bool(LedgerEntry const&)> const& filterEntry,
    std::function<bool(LedgerEntry const&)> const& acceptEntry,
    UnorderedSet<Hash>& processedEntries)
{
    ZoneScoped;

    using namespace std::chrono;
    medida::Timer timer;

    bool stopIteration = false;
    timer.Time([&]() {
        for (LiveBucketInputIterator in(b); in; ++in)
        {
            BucketEntry const& e = *in;
            if (e.type() == LIVEENTRY || e.type() == INITENTRY)
            {
                auto const& liveEntry = e.liveEntry();
                if (minLedger && liveEntry.lastModifiedLedgerSeq < *minLedger)
                {
                    stopIteration = true;
                    continue;
                }
                if (!filterEntry(liveEntry))
                {
                    continue;
                }
                if (!processedEntries
                         .insert(xdrBlake2(LedgerEntryKey(liveEntry)))
                         .second)
                {
                    continue;
                }
                if (!acceptEntry(liveEntry))
                {
                    stopIteration = true;
                    break;
                }
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
                processedEntries.insert(xdrBlake2(e.deadEntry()));
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

static bool
visitAllEntriesInBucket(
    std::shared_ptr<LiveBucket const> b, std::string const& name,
    std::optional<int64_t> minLedger,
    std::function<bool(LedgerEntry const&)> const& filterEntry,
    std::function<bool(LedgerEntry const&)> const& acceptEntry)
{
    ZoneScoped;

    using namespace std::chrono;
    medida::Timer timer;

    bool stopIteration = false;
    timer.Time([&]() {
        for (LiveBucketInputIterator in(b); in; ++in)
        {
            BucketEntry const& e = *in;
            if (e.type() == LIVEENTRY || e.type() == INITENTRY)
            {
                auto const& liveEntry = e.liveEntry();
                if (minLedger && liveEntry.lastModifiedLedgerSeq < *minLedger)
                {
                    stopIteration = true;
                    continue;
                }
                if (filterEntry(e.liveEntry()))
                {
                    if (!acceptEntry(e.liveEntry()))
                    {
                        stopIteration = true;
                        break;
                    }
                }
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
BucketManager::visitLedgerEntries(
    HistoryArchiveState const& has, std::optional<int64_t> minLedger,
    std::function<bool(LedgerEntry const&)> const& filterEntry,
    std::function<bool(LedgerEntry const&)> const& acceptEntry,
    bool includeAllStates)
{
    ZoneScoped;

    UnorderedSet<Hash> deletedEntries;
    std::vector<std::pair<Hash, std::string>> hashes;
    for (uint32_t i = 0; i < LiveBucketList::kNumLevels; ++i)
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
            auto b = getBucketByHashInternal(pair.first, mSharedLiveBuckets);
            if (!b)
            {
                throw std::runtime_error(std::string("missing bucket: ") +
                                         binToHex(pair.first));
            }
            bool continueIteration =
                includeAllStates
                    ? visitAllEntriesInBucket(b, pair.second, minLedger,
                                              filterEntry, acceptEntry)
                    : visitLiveEntriesInBucket(b, pair.second, minLedger,
                                               filterEntry, acceptEntry,
                                               deletedEntries);
            if (!continueIteration)
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
BucketManager::scheduleVerifyReferencedBucketsWork(
    HistoryArchiveState const& has)
{
    releaseAssert(threadIsMain());
    std::set<Hash> hashes = getAllReferencedBuckets(has);
    std::vector<std::shared_ptr<BasicWork>> seq;
    for (auto const& h : hashes)
    {
        if (isZero(h))
        {
            continue;
        }

        // TODO: Update verify to for ArchiveBucket
        // Dependency: HAS supports Hot Archive BucketList
        auto b = getBucketByHashInternal(h, mSharedLiveBuckets);
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
BucketManager::getConfig() const
{
    return mConfig;
}

void
BucketManager::reportBucketEntryCountMetrics()
{
    auto bucketEntryCounters = mLiveBucketList->sumBucketEntryCounters();
    for (auto [type, count] : bucketEntryCounters.entryTypeCounts)
    {
        auto countCounter = mBucketListEntryCountCounters.find(type);
        if (countCounter == mBucketListEntryCountCounters.end())
        {
            auto typeString = toString(type);
            countCounter =
                mBucketListEntryCountCounters
                    .emplace(type,
                             mApp.getMetrics().NewCounter(
                                 {"bucketlist", "entryCounts", typeString}))
                    .first;
        }
        countCounter->second.set_count(count);

        auto sizeCounter = mBucketListEntrySizeCounters.find(type);
        if (sizeCounter == mBucketListEntrySizeCounters.end())
        {
            auto typeString = toString(type);
            sizeCounter =
                mBucketListEntrySizeCounters
                    .emplace(type,
                             mApp.getMetrics().NewCounter(
                                 {"bucketlist", "entrySizes", typeString}))
                    .first;
        }
        sizeCounter->second.set_count(
            bucketEntryCounters.entryTypeSizes.at(type));
    }
}

template void BucketManager::maybeSetIndex<LiveBucket>(
    std::shared_ptr<LiveBucket> b,
    std::unique_ptr<LiveBucket::IndexT const>&& index);
template void BucketManager::maybeSetIndex<HotArchiveBucket>(
    std::shared_ptr<HotArchiveBucket> b,
    std::unique_ptr<HotArchiveBucket::IndexT const>&& index);
template medida::Meter& BucketManager::getBloomMissMeter<LiveBucket>() const;
template medida::Meter& BucketManager::getBloomLookupMeter<LiveBucket>() const;
template medida::Meter&
BucketManager::getBloomMissMeter<HotArchiveBucket>() const;
template medida::Meter&
BucketManager::getBloomLookupMeter<HotArchiveBucket>() const;
}
