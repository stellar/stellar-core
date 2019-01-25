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
class RawBucket;
using Bucket = std::shared_ptr<const RawBucket>;
struct HistoryArchiveState;
struct LedgerHeaderHistoryEntry;

class ApplyBucketsWork : public Work
{
    std::map<std::string, Bucket> const& mBuckets;
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
    Bucket mSnapBucket;
    Bucket mCurrBucket;
    std::unique_ptr<BucketApplicator> mSnapApplicator;
    std::unique_ptr<BucketApplicator> mCurrApplicator;

    medida::Meter& mBucketApplyStart;
    medida::Meter& mBucketApplySuccess;
    medida::Meter& mBucketApplyFailure;

    Bucket getBucket(std::string const& bucketHash);
    BucketLevel& getBucketLevel(uint32_t level);
    void advance(BucketApplicator& applicator);

  public:
    ApplyBucketsWork(Application& app, WorkParent& parent,
                     std::map<std::string, Bucket> const& buckets,
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
