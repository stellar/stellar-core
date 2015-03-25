#pragma once

#include "bucket/BucketList.h"
#include "bucket/BucketManager.h"
#include "generated/StellarXDR.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

namespace medida
{
class Timer;
class Meter;
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
    std::map<std::string, std::shared_ptr<Bucket>> mSharedBuckets;
    mutable std::recursive_mutex mBucketMutex;
    std::unique_ptr<std::string> mLockedBucketDir;
    medida::Meter& mBucketObjectInsert;
    medida::Meter& mBucketByteInsert;
    medida::Timer& mBucketAddBatch;
    medida::Timer& mBucketSnapMerge;

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
    void snapshotLedger(LedgerHeader& currentHeader);
    void assumeState(HistoryArchiveState const& has);
};

}
