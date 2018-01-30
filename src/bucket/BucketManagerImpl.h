#pragma once

#include "bucket/BucketList.h"
#include "bucket/BucketManager.h"
#include "overlay/StellarXDR.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

namespace medida
{
class Timer;
class Meter;
class Counter;
}

namespace stellar
{

class TmpDir;
class Application;
class Bucket;
class BucketList;
struct HistoryArchiveState;

class BucketManagerImpl : public BucketManager
{
    static std::string const kLockFilename;

    Application& mApp;
    BucketList mBucketList;
    std::unique_ptr<TmpDir> mWorkDir;
    std::map<Hash, std::shared_ptr<Bucket>> mSharedBuckets;
    mutable std::recursive_mutex mBucketMutex;
    std::unique_ptr<std::string> mLockedBucketDir;
    medida::Meter& mBucketObjectInsert;
    medida::Meter& mBucketByteInsert;
    medida::Timer& mBucketAddBatch;
    medida::Timer& mBucketSnapMerge;
    medida::Counter& mSharedBucketsSize;

  protected:
    void calculateSkipValues(LedgerHeader& currentHeader);
    std::string bucketFilename(std::string const& bucketHexHash);
    std::string bucketFilename(Hash const& hash);

  public:
    BucketManagerImpl(Application& app);
    ~BucketManagerImpl() override;
    std::string const& getTmpDir() override;
    std::string const& getBucketDir() override;
    BucketList& getBucketList() override;
    medida::Timer& getMergeTimer() override;
    std::shared_ptr<Bucket> adoptFileAsBucket(std::string const& filename,
                                              uint256 const& hash,
                                              size_t nObjects,
                                              size_t nBytes) override;
    std::shared_ptr<Bucket> getBucketByHash(uint256 const& hash) override;

    void forgetUnreferencedBuckets() override;
    void addBatch(Application& app, uint32_t currLedger,
                  std::vector<LedgerEntry> const& liveEntries,
                  std::vector<LedgerKey> const& deadEntries) override;
    void snapshotLedger(LedgerHeader& currentHeader) override;

    std::vector<std::string>
    checkForMissingBucketsFiles(HistoryArchiveState const& has) override;
    void assumeState(HistoryArchiveState const& has) override;
    void shutdown() override;
};

#define SKIP_1 50
#define SKIP_2 5000
#define SKIP_3 50000
#define SKIP_4 500000
}
