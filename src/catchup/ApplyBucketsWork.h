// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "bucket/BucketApplicator.h"
#include "work/Work.h"

namespace medida
{
class Meter;
}

namespace stellar
{

class BucketLevel;
class BucketList;
class Bucket;
struct HistoryArchiveState;
struct LedgerHeaderHistoryEntry;

class ApplyBucketsWork : public BasicWork
{
    std::map<std::string, std::shared_ptr<Bucket>> const& mBuckets;
    HistoryArchiveState const& mApplyState;
    bool mHaveCheckedApplyStateValidity{false};

    bool mApplying{false};
    size_t mTotalBuckets{0};
    size_t mAppliedBuckets{0};
    size_t mAppliedEntries{0};
    size_t mTotalSize{0};
    size_t mAppliedSize{0};
    size_t mLastAppliedSizeMb{0};
    size_t mLastPos{0};
    uint32_t mLevel{0};
    uint32_t mMaxProtocolVersion{0};
    std::shared_ptr<Bucket const> mSnapBucket;
    std::shared_ptr<Bucket const> mCurrBucket;
    std::unique_ptr<BucketApplicator> mSnapApplicator;
    std::unique_ptr<BucketApplicator> mCurrApplicator;

    medida::Meter& mBucketApplyStart;
    medida::Meter& mBucketApplySuccess;
    medida::Meter& mBucketApplyFailure;
    BucketApplicator::Counters mCounters;

    // With FIRST_PROTOCOL_SHADOWS_REMOVED or higher, when buckets are applied,
    // we do not have resolved outputs before applying transactions and joining
    // the network. If online catchup does not wait for merges to be resolved,
    // marks a node as "in sync", and begins closing ledgers, a large (but not
    // yet finished) merge might be needed at an applied ledger. At that point,
    // `closeLedger` will block waiting for the merge to resolve. If the delay
    // is long enough, node might go out of sync. Specifically, this applies to
    // the following scenarios:
    // - Large merge is needed during ApplyLedgerChainWork
    // - Large merge is needed during syncing ledgers replay
    // - Large merge is needed after successful catchup, during normal in-sync
    // ledger close
    // To prevent this, wait for restarted merges to resolve before proceeding.
    // This approach conservatively waits for all merges regardless of HAS
    // ledger number, and number of ledgers to replay.

    bool const mResolveMerges;
    std::unique_ptr<VirtualTimer> mDelayTimer;

    void advance(std::string const& name, BucketApplicator& applicator);
    std::shared_ptr<Bucket const> getBucket(std::string const& bucketHash);
    BucketLevel& getBucketLevel(uint32_t level);
    void startLevel();
    bool isLevelComplete();

  public:
    ApplyBucketsWork(
        Application& app,
        std::map<std::string, std::shared_ptr<Bucket>> const& buckets,
        HistoryArchiveState const& applyState, uint32_t maxProtocolVersion,
        bool resolveMerges);
    ~ApplyBucketsWork() = default;

  protected:
    void onReset() override;
    BasicWork::State onRun() override;
    bool
    onAbort() override
    {
        return true;
    };
    void onFailureRaise() override;
    void onFailureRetry() override;
    void onSuccess() override;
};
}
