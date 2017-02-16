// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "work/Work.h"

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
    std::map<std::string, std::shared_ptr<Bucket>>& mBuckets;
    HistoryArchiveState& mApplyState;
    LedgerHeaderHistoryEntry const& mFirstVerified;

    bool mApplying;
    size_t mLevel;
    std::shared_ptr<Bucket> mSnapBucket;
    std::shared_ptr<Bucket> mCurrBucket;
    std::unique_ptr<BucketApplicator> mSnapApplicator;
    std::unique_ptr<BucketApplicator> mCurrApplicator;

    std::shared_ptr<Bucket> getBucket(std::string const& bucketHash);
    BucketLevel& getBucketLevel(size_t level);
    BucketList& getBucketList();

  public:
    ApplyBucketsWork(Application& app, WorkParent& parent,
                     std::map<std::string, std::shared_ptr<Bucket>>& buckets,
                     HistoryArchiveState& applyState,
                     LedgerHeaderHistoryEntry const& firstVerified);
    ~ApplyBucketsWork();

    void onReset() override;
    void onStart() override;
    void onRun() override;
    Work::State onSuccess() override;
};
}
