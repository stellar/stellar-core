// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/ApplyBucketsWork.h"
#include "bucket/BucketApplicator.h"
#include "bucket/BucketManager.h"
#include "bucket/LiveBucket.h"
#include "bucket/LiveBucketList.h"
#include "catchup/AssumeStateWork.h"
#include "catchup/IndexBucketsWork.h"
#include "catchup/LedgerApplyManager.h"
#include "crypto/Hex.h"
#include "crypto/SecretKey.h"
#include "historywork/Progress.h"
#include "invariant/InvariantManager.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "main/Application.h"
#include "transactions/TransactionUtils.h"
#include "util/GlobalChecks.h"
#include <Tracy.hpp>
#include <fmt/format.h>

namespace stellar
{

class TempLedgerVersionSetter : NonMovableOrCopyable
{
    Application& mApp;
    uint32 mOldVersion;

    void
    setVersion(uint32 ver)
    {
        LedgerTxn ltx(mApp.getLedgerTxnRoot());
        auto header = ltx.loadHeader();
        mOldVersion = header.current().ledgerVersion;
        header.current().ledgerVersion = ver;
        ltx.commit();
    }

  public:
    TempLedgerVersionSetter(Application& app, uint32 newVersion) : mApp(app)
    {
        setVersion(newVersion);
    }
    ~TempLedgerVersionSetter()
    {
        setVersion(mOldVersion);
    }
};

ApplyBucketsWork::ApplyBucketsWork(
    Application& app,
    std::map<std::string, std::shared_ptr<LiveBucket>> const& buckets,
    HistoryArchiveState const& applyState, uint32_t maxProtocolVersion)
    : Work(app, "apply-buckets", BasicWork::RETRY_NEVER)
    , mBuckets(buckets)
    , mApplyState(applyState)
    , mTotalSize(0)
    , mLevel(0)
    , mMaxProtocolVersion(maxProtocolVersion)
    , mCounters(app.getClock().now())
    , mIsApplyInvariantEnabled(
          app.getInvariantManager().isBucketApplyInvariantEnabled())
{
}

std::shared_ptr<LiveBucket>
ApplyBucketsWork::getBucket(std::string const& hash)
{
    auto i = mBuckets.find(hash);
    auto b = (i != mBuckets.end())
                 ? i->second
                 : mApp.getBucketManager().getBucketByHash<LiveBucket>(
                       hexToBin256(hash));
    releaseAssert(b);
    return b;
}

void
ApplyBucketsWork::doReset()
{
    ZoneScoped;
    CLOG_INFO(History, "Applying buckets");

    mTotalBuckets = 0;
    mAppliedBuckets = 0;
    mAppliedEntries = 0;
    mTotalSize = 0;
    mAppliedSize = 0;
    mLastAppliedSizeMb = 0;
    mLastPos = 0;
    mBucketToApplyIndex = 0;
    mMinProtocolVersionSeen = UINT32_MAX;
    mSeenKeysBeforeApply.clear();
    mSeenKeys.clear();
    mBucketsToApply.clear();
    mBucketApplicator.reset();

    if (!isAborting())
    {
        // The current size of this set is 1.6 million during BucketApply
        // (as of 12/20/23). There's not a great way to estimate this, so
        // reserving with some extra wiggle room
        static const size_t estimatedOfferKeyCount = 2'000'000;
        mSeenKeys.reserve(estimatedOfferKeyCount);

        auto addBucket = [this](std::shared_ptr<LiveBucket> const& bucket) {
            if (bucket->getSize() > 0)
            {
                mTotalBuckets++;
                mTotalSize += bucket->getSize();
            }
            mBucketsToApply.emplace_back(bucket);
        };

        // We iterate through the live BucketList in
        // order (i.e. L0 curr, L0 snap, L1 curr, etc) as we are just applying
        // offers (and can keep track of all seen keys).
        for (auto const& hsb : mApplyState.currentBuckets)
        {
            addBucket(getBucket(hsb.curr));
            addBucket(getBucket(hsb.snap));
        }

        // estimate the number of ledger entries contained in those buckets
        // use accounts as a rough approximator as to overestimate a bit
        // (default BucketEntry contains a default AccountEntry)
        size_t const estimatedLedgerEntrySize =
            xdr::xdr_traits<BucketEntry>::serial_size(BucketEntry{});
        size_t const totalLECount = mTotalSize / estimatedLedgerEntrySize;
        CLOG_INFO(History, "ApplyBuckets estimated {} ledger entries",
                  totalLECount);
        mApp.getLedgerTxnRoot().prepareNewObjects(totalLECount);
    }
}

bool
ApplyBucketsWork::appliedAllBuckets() const
{
    return mBucketToApplyIndex == mBucketsToApply.size();
}

void
ApplyBucketsWork::startBucket()
{
    ZoneScoped;
    auto bucket = mBucketsToApply.at(mBucketToApplyIndex);
    mMinProtocolVersionSeen =
        std::min(mMinProtocolVersionSeen, bucket->getBucketVersion());

    // Take a snapshot of seen keys before applying the bucket, only if
    // invariants are enabled since this is expensive.
    if (mIsApplyInvariantEnabled)
    {
        mSeenKeysBeforeApply = mSeenKeys;
    }

    // Create a new applicator for the bucket.
    mBucketApplicator = std::make_unique<BucketApplicator>(
        mApp, mMaxProtocolVersion, mMinProtocolVersionSeen, mLevel, bucket,
        mSeenKeys);
}

void
ApplyBucketsWork::prepareForNextBucket()
{
    ZoneScoped;
    mBucketApplicator.reset();
    mApp.getLedgerApplyManager().bucketsApplied();
    mBucketToApplyIndex++;
    // If mBucketToApplyIndex is even, we are progressing to the next
    // level
    if (mBucketToApplyIndex % 2 == 0)
    {
        ++mLevel;
    }
}

// We iterate through the live BucketList either in-order (level 0 curr, level 0
// snap, level 1 curr, etc). We keep track of the keys we have already
// seen, and only apply an entry to the DB if it has not been seen before. This
// allows us to perform a single write to the DB and ensure that only the newest
// version is written.
//
BasicWork::State
ApplyBucketsWork::doWork()
{
    ZoneScoped;

    // Step 1: index buckets. Step 2: apply buckets. Step 3: assume state
    if (!mIndexBucketsWork)
    {
        // Spawn indexing work for the first time
        mIndexBucketsWork = addWork<IndexBucketsWork>(mBucketsToApply);
        return State::WORK_RUNNING;
    }

    else if (mIndexBucketsWork->getState() != BasicWork::State::WORK_SUCCESS)
    {
        // Exit early if indexing work is still running, or failed
        return mIndexBucketsWork->getState();
    }

    if (!mAssumeStateWork)
    {
        // Step 2: apply buckets.
        auto isCurr = mBucketToApplyIndex % 2 == 0;
        if (mBucketApplicator)
        {
            TempLedgerVersionSetter tlvs(mApp, mMaxProtocolVersion);
            // Only advance the applicator if there are entries to apply.
            if (*mBucketApplicator)
            {
                advance(isCurr ? "curr" : "snap", *mBucketApplicator);
                return State::WORK_RUNNING;
            }
            // Application complete, check invariants and prepare for next
            // bucket. Applying a bucket updates mSeenKeys with the keys applied
            // by that bucket, so we need to provide a copy of the keys before
            // application to the invariant check.
            mApp.getInvariantManager().checkOnBucketApply(
                mBucketsToApply.at(mBucketToApplyIndex),
                mApplyState.currentLedger, mLevel, isCurr,
                mSeenKeysBeforeApply);
            prepareForNextBucket();
        }
        if (!appliedAllBuckets())
        {
            CLOG_DEBUG(History, "ApplyBuckets : starting level: {}, {}", mLevel,
                       isCurr ? "curr" : "snap");
            startBucket();
            return State::WORK_RUNNING;
        }

        CLOG_INFO(History, "ApplyBuckets : done, assuming state");

        // After all buckets applied, spawn assumeState work
        mAssumeStateWork =
            addWork<AssumeStateWork>(mApplyState, mMaxProtocolVersion,
                                     /* restartMerges */ true);
    }

    return checkChildrenStatus();
}

void
ApplyBucketsWork::advance(std::string const& bucketName,
                          BucketApplicator& applicator)
{
    ZoneScoped;
    releaseAssert(applicator);
    releaseAssert(mTotalSize != 0);
    auto sz = applicator.advance(mCounters);
    mAppliedEntries += sz;
    mCounters.logDebug(bucketName, mLevel, mApp.getClock().now());

    auto log = false;
    if (applicator)
    {
        mAppliedSize += (applicator.pos() - mLastPos);
        mLastPos = applicator.pos();
    }
    else
    {
        mAppliedSize += (applicator.size() - mLastPos);
        mAppliedBuckets++;
        mLastPos = 0;
        log = true;
        mCounters.logInfo(bucketName, mLevel, mApp.getClock().now());
        mCounters.reset(mApp.getClock().now());
    }

    auto appliedSizeMb = mAppliedSize / 1024 / 1024;
    if (appliedSizeMb > mLastAppliedSizeMb)
    {
        log = true;
        mLastAppliedSizeMb = appliedSizeMb;
    }

    if (log)
    {
        CLOG_INFO(
            Bucket, "Bucket-apply: {} entries in {}/{} in {}/{} files ({}%)",
            mAppliedEntries, formatSize(mAppliedSize), formatSize(mTotalSize),
            mAppliedBuckets, mTotalBuckets, (100 * mAppliedSize / mTotalSize));
    }
}

std::string
ApplyBucketsWork::getStatus() const
{
    // This status string only applies to step 2 when we actually apply the
    // buckets.
    bool doneIndexing = mIndexBucketsWork && mIndexBucketsWork->isDone();
    if (doneIndexing && !mSpawnedAssumeStateWork)
    {
        auto size = mTotalSize == 0 ? 0 : (100 * mAppliedSize / mTotalSize);
        return fmt::format(
            FMT_STRING("Applying buckets {:d}%. Currently on level {:d}"), size,
            mLevel);
    }

    return Work::getStatus();
}
}
