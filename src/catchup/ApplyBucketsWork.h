// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "work/Work.h"

namespace medida
{
class Meter;
}

namespace stellar
{

class BucketApplicator;
class BucketLevel;
class BucketList;
class Bucket;
struct HistoryArchiveState;
struct LedgerHeaderHistoryEntry;

class ApplyBucketsWork : public Work
{
    std::map<std::string, std::shared_ptr<Bucket>> const& mBuckets;
    const HistoryArchiveState& mApplyState;

    bool mApplying;
    size_t mTotalBuckets;
    size_t mAppliedBuckets;
    size_t mAppliedEntries;
    size_t mTotalSize;
    size_t mAppliedSize;
    size_t mLastAppliedSizeMb;
    size_t mLastPos;
    uint32_t mLevel;
    std::shared_ptr<Bucket const> mSnapBucket;
    std::shared_ptr<Bucket const> mCurrBucket;
    std::unique_ptr<BucketApplicator> mSnapApplicator;
    std::unique_ptr<BucketApplicator> mCurrApplicator;

    medida::Meter& mBucketApplyStart;
    medida::Meter& mBucketApplySuccess;
    medida::Meter& mBucketApplyFailure;

    std::shared_ptr<Bucket const> getBucket(std::string const& bucketHash);
    BucketLevel& getBucketLevel(uint32_t level);
    void advance(BucketApplicator& applicator);

  public:
    ApplyBucketsWork(
        Application& app, WorkParent& parent,
        std::map<std::string, std::shared_ptr<Bucket>> const& buckets,
        HistoryArchiveState const& applyState);
    ~ApplyBucketsWork();

    void onReset() override;
    void onStart() override;
    void onRun() override;
    Work::State onSuccess() override;
    void onFailureRetry() override;
    void onFailureRaise() override;
};
}
