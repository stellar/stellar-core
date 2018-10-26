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

class ApplyBucketsWork : public BasicWork
{
    std::map<std::string, std::shared_ptr<Bucket>> const& mBuckets;
    HistoryArchiveState const& mApplyState;

    bool mApplying;
    uint32_t mLevel;
    std::shared_ptr<Bucket const> mSnapBucket;
    std::shared_ptr<Bucket const> mCurrBucket;
    std::unique_ptr<BucketApplicator> mSnapApplicator;
    std::unique_ptr<BucketApplicator> mCurrApplicator;

    medida::Meter& mBucketApplyStart;
    medida::Meter& mBucketApplySuccess;
    medida::Meter& mBucketApplyFailure;

    bool mLevelComplete{true};

    std::shared_ptr<Bucket const>
    getBucket(std::string const& bucketHash) const;
    BucketLevel& getBucketLevel(uint32_t level) const;
    void startLevel();

  public:
    ApplyBucketsWork(
        Application& app,
        std::map<std::string, std::shared_ptr<Bucket>> const& buckets,
        HistoryArchiveState const& applyState);
    ~ApplyBucketsWork();

  protected:
    void onReset() override;
    BasicWork::State onRun() override;
    void onFailureRaise() override;
    void onFailureRetry() override;
};
}
