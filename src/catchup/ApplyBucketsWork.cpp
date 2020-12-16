// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/ApplyBucketsWork.h"
#include "bucket/Bucket.h"
#include "bucket/BucketApplicator.h"
#include "bucket/BucketList.h"
#include "bucket/BucketManager.h"
#include "catchup/CatchupManager.h"
#include "crypto/Hex.h"
#include "crypto/SecretKey.h"
#include "history/HistoryArchive.h"
#include "historywork/Progress.h"
#include "invariant/InvariantManager.h"
#include "ledger/LedgerTxn.h"
#include "main/Application.h"
#include <Tracy.hpp>
#include <fmt/format.h>
#include <medida/meter.h>
#include <medida/metrics_registry.h>

namespace stellar
{

ApplyBucketsWork::ApplyBucketsWork(
    Application& app,
    std::map<std::string, std::shared_ptr<Bucket>> const& buckets,
    HistoryArchiveState const& applyState, uint32_t maxProtocolVersion)
    : BasicWork(app, "apply-buckets", BasicWork::RETRY_NEVER)
    , mBuckets(buckets)
    , mApplyState(applyState)
    , mApplying(false)
    , mTotalSize(0)
    , mLevel(BucketList::kNumLevels - 1)
    , mMaxProtocolVersion(maxProtocolVersion)
    , mBucketApplyStart(app.getMetrics().NewMeter(
          {"history", "bucket-apply", "start"}, "event"))
    , mBucketApplySuccess(app.getMetrics().NewMeter(
          {"history", "bucket-apply", "success"}, "event"))
    , mBucketApplyFailure(app.getMetrics().NewMeter(
          {"history", "bucket-apply", "failure"}, "event"))
    , mCounters(app.getClock().now())
{
}

BucketLevel&
ApplyBucketsWork::getBucketLevel(uint32_t level)
{
    return mApp.getBucketManager().getBucketList().getLevel(level);
}

std::shared_ptr<Bucket const>
ApplyBucketsWork::getBucket(std::string const& hash)
{
    auto i = mBuckets.find(hash);
    auto b = (i != mBuckets.end())
                 ? i->second
                 : mApp.getBucketManager().getBucketByHash(hexToBin256(hash));
    assert(b);
    return b;
}

void
ApplyBucketsWork::onReset()
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

    if (!isAborting())
    {
        auto addBucket = [this](std::shared_ptr<Bucket const> const& bucket) {
            if (bucket->getSize() > 0)
            {
                mTotalBuckets++;
                mTotalSize += bucket->getSize();
            }
        };

        for (auto const& hsb : mApplyState.currentBuckets)
        {
            addBucket(getBucket(hsb.snap));
            addBucket(getBucket(hsb.curr));
        }
    }

    mLevel = BucketList::kNumLevels - 1;
    mApplying = false;

    mSnapBucket.reset();
    mCurrBucket.reset();
    mSnapApplicator.reset();
    mCurrApplicator.reset();
}

void
ApplyBucketsWork::startLevel()
{
    ZoneScoped;
    assert(isLevelComplete());

    CLOG_DEBUG(History, "ApplyBuckets : starting level {}", mLevel);
    auto& level = getBucketLevel(mLevel);
    HistoryStateBucket const& i = mApplyState.currentBuckets.at(mLevel);

    bool applySnap = (i.snap != binToHex(level.getSnap()->getHash()));
    bool applyCurr = (i.curr != binToHex(level.getCurr()->getHash()));

    if (!mApplying && !mApp.getConfig().MODE_USES_IN_MEMORY_LEDGER &&
        (applySnap || applyCurr))
    {
        uint32_t oldestLedger = applySnap
                                    ? BucketList::oldestLedgerInSnap(
                                          mApplyState.currentLedger, mLevel)
                                    : BucketList::oldestLedgerInCurr(
                                          mApplyState.currentLedger, mLevel);
        auto& lsRoot = mApp.getLedgerTxnRoot();
        lsRoot.deleteObjectsModifiedOnOrAfterLedger(oldestLedger);
    }

    if (mApplying || applySnap)
    {
        mSnapBucket = getBucket(i.snap);
        mSnapApplicator = std::make_unique<BucketApplicator>(
            mApp, mMaxProtocolVersion, mSnapBucket);
        CLOG_DEBUG(History, "ApplyBuckets : starting level[{}].snap = {}",
                   mLevel, i.snap);
        mApplying = true;
        mBucketApplyStart.Mark();
    }
    if (mApplying || applyCurr)
    {
        mCurrBucket = getBucket(i.curr);
        mCurrApplicator = std::make_unique<BucketApplicator>(
            mApp, mMaxProtocolVersion, mCurrBucket);
        CLOG_DEBUG(History, "ApplyBuckets : starting level[{}].curr = {}",
                   mLevel, i.curr);
        mApplying = true;
        mBucketApplyStart.Mark();
    }
}

BasicWork::State
ApplyBucketsWork::onRun()
{
    ZoneScoped;
    if (!mHaveCheckedApplyStateValidity && mLevel == BucketList::kNumLevels - 1)
    {
        if (!mApplyState.containsValidBuckets(mApp))
        {
            CLOG_ERROR(History, "Malformed HAS: unable to apply buckets");
            return State::WORK_FAILURE;
        }
        mHaveCheckedApplyStateValidity = true;
    }

    // Check if we're at the beginning of the new level
    if (isLevelComplete())
    {
        startLevel();
    }

    // The structure of these if statements is motivated by the following:
    // 1. mCurrApplicator should never be advanced if mSnapApplicator is
    //    not false. Otherwise it is possible for curr to modify the
    //    database when the invariants for snap are checked.
    // 2. There is no reason to advance mSnapApplicator or mCurrApplicator
    //    if there is nothing to be applied.
    if (mSnapApplicator)
    {
        if (*mSnapApplicator)
        {
            advance("snap", *mSnapApplicator);
            return State::WORK_RUNNING;
        }
        mApp.getInvariantManager().checkOnBucketApply(
            mSnapBucket, mApplyState.currentLedger, mLevel, false);
        mSnapApplicator.reset();
        mSnapBucket.reset();
        mBucketApplySuccess.Mark();
    }
    if (mCurrApplicator)
    {
        if (*mCurrApplicator)
        {
            advance("curr", *mCurrApplicator);
            return State::WORK_RUNNING;
        }
        mApp.getInvariantManager().checkOnBucketApply(
            mCurrBucket, mApplyState.currentLedger, mLevel, true);
        mCurrApplicator.reset();
        mCurrBucket.reset();
        mBucketApplySuccess.Mark();
    }

    if (mLevel != 0)
    {
        --mLevel;
        CLOG_DEBUG(History, "ApplyBuckets : starting next level: {}", mLevel);
        return State::WORK_RUNNING;
    }

    CLOG_INFO(History, "ApplyBuckets : done, restarting merges");
    mApp.getBucketManager().assumeState(mApplyState, mMaxProtocolVersion);

    return State::WORK_SUCCESS;
}

void
ApplyBucketsWork::advance(std::string const& bucketName,
                          BucketApplicator& applicator)
{
    ZoneScoped;
    assert(applicator);
    assert(mTotalSize != 0);
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

bool
ApplyBucketsWork::isLevelComplete()
{
    return !(mApplying) || !(mSnapApplicator || mCurrApplicator);
}

void
ApplyBucketsWork::onFailureRaise()
{
    mBucketApplyFailure.Mark();
}

void
ApplyBucketsWork::onFailureRetry()
{
    mBucketApplyFailure.Mark();
}

std::string
ApplyBucketsWork::getStatus() const
{
    return fmt::format("Applying buckets {}%. Currently on level {}",
                       (100 * mAppliedSize / mTotalSize), mLevel);
}
}
