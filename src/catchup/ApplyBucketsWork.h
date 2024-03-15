// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "bucket/BucketApplicator.h"
#include "work/Work.h"

namespace stellar
{

class BucketLevel;
class BucketList;
class Bucket;
struct HistoryArchiveState;
struct LedgerHeaderHistoryEntry;

class ApplyBucketsWork : public Work
{
    std::map<std::string, std::shared_ptr<Bucket>> const& mBuckets;
    HistoryArchiveState const& mApplyState;
    std::function<bool(LedgerEntryType)> mEntryTypeFilter;

    bool mApplying{false};
    bool mSpawnedAssumeStateWork{false};
    bool mSpawnedIndexBucketsWork{false};
    bool mFinishedIndexBucketsWork{false};
    size_t mTotalBuckets{0};
    size_t mAppliedBuckets{0};
    size_t mAppliedEntries{0};
    size_t mTotalSize{0};
    size_t mAppliedSize{0};
    size_t mLastAppliedSizeMb{0};
    size_t mLastPos{0};
    bool const mOffersOnly;
    uint32_t mLevel{0};
    uint32_t mMaxProtocolVersion{0};
    uint32_t mMinProtocolVersionSeen{UINT32_MAX};
    std::shared_ptr<Bucket const> mFirstBucket;
    std::shared_ptr<Bucket const> mSecondBucket;
    std::unique_ptr<BucketApplicator> mFirstBucketApplicator;
    std::unique_ptr<BucketApplicator> mSecondBucketApplicator;
    UnorderedSet<LedgerKey> mSeenKeys;
    std::vector<std::shared_ptr<Bucket>> mBucketsToIndex;

    BucketApplicator::Counters mCounters;

    void advance(std::string const& name, BucketApplicator& applicator);
    std::shared_ptr<Bucket> getBucket(std::string const& bucketHash);
    BucketLevel& getBucketLevel(uint32_t level);
    void startLevel();
    bool isLevelComplete();

    bool mDelayChecked{false};

    static uint32_t startingLevel(bool offersOnly);
    uint32_t nextLevel() const;
    bool appliedAllLevels() const;

  public:
    ApplyBucketsWork(
        Application& app,
        std::map<std::string, std::shared_ptr<Bucket>> const& buckets,
        HistoryArchiveState const& applyState, uint32_t maxProtocolVersion);
    ApplyBucketsWork(
        Application& app,
        std::map<std::string, std::shared_ptr<Bucket>> const& buckets,
        HistoryArchiveState const& applyState, uint32_t maxProtocolVersion,
        std::function<bool(LedgerEntryType)> onlyApply);
    ~ApplyBucketsWork() = default;

    std::string getStatus() const override;

  protected:
    void doReset() override;
    BasicWork::State doWork() override;
};
}
