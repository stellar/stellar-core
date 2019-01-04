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
#include "util/format.h"
#include <medida/meter.h>
#include <medida/metrics_registry.h>

namespace stellar
{

ApplyBucketsWork::ApplyBucketsWork(
    Application& app, WorkParent& parent,
    std::map<std::string, std::shared_ptr<Bucket>> const& buckets,
    HistoryArchiveState const& applyState)
    : Work(app, parent, std::string("apply-buckets"))
    , mBuckets(buckets)
    , mApplyState(applyState)
    , mApplying(false)
    , mTotalSize(0)
    , mLevel(BucketList::kNumLevels - 1)
    , mBucketApplyStart(app.getMetrics().NewMeter(
          {"history", "bucket-apply", "start"}, "event"))
    , mBucketApplySuccess(app.getMetrics().NewMeter(
          {"history", "bucket-apply", "success"}, "event"))
    , mBucketApplyFailure(app.getMetrics().NewMeter(
          {"history", "bucket-apply", "failure"}, "event"))
{
}

ApplyBucketsWork::~ApplyBucketsWork()
{
    clearChildren();
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
    mTotalBuckets = 0;
    mAppliedBuckets = 0;
    mAppliedEntries = 0;
    mTotalSize = 0;
    mAppliedSize = 0;
    mLastAppliedSizeMb = 0;
    mLastPos = 0;

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

    mLevel = BucketList::kNumLevels - 1;
    mApplying = false;
    mSnapBucket.reset();
    mCurrBucket.reset();
    mSnapApplicator.reset();
    mCurrApplicator.reset();
}

void
ApplyBucketsWork::onStart()
{
    auto& level = getBucketLevel(mLevel);
    HistoryStateBucket const& i = mApplyState.currentBuckets.at(mLevel);

    bool applySnap = (i.snap != binToHex(level.getSnap()->getHash()));
    bool applyCurr = (i.curr != binToHex(level.getCurr()->getHash()));
    if (!mApplying && (applySnap || applyCurr))
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
        mSnapApplicator = std::make_unique<BucketApplicator>(mApp, mSnapBucket);
        CLOG(DEBUG, "History") << "ApplyBuckets : starting level[" << mLevel
                               << "].snap = " << i.snap;
        mApplying = true;
        mBucketApplyStart.Mark();
    }
    if (mApplying || applyCurr)
    {
        mCurrBucket = getBucket(i.curr);
        mCurrApplicator = std::make_unique<BucketApplicator>(mApp, mCurrBucket);
        CLOG(DEBUG, "History") << "ApplyBuckets : starting level[" << mLevel
                               << "].curr = " << i.curr;
        mApplying = true;
        mBucketApplyStart.Mark();
    }
}

void
ApplyBucketsWork::onRun()
{
    // The structure of these if statements is motivated by the following:
    // 1. mCurrApplicator should never be advanced if mSnapApplicator is
    //    not false. Otherwise it is possible for curr to modify the
    //    database when the invariants for snap are checked.
    // 2. There is no reason to advance mSnapApplicator or mCurrApplicator
    //    if there is nothing to be applied.
    if (mSnapApplicator)
    {
        advance(*mSnapApplicator);
    }
    else if (mCurrApplicator)
    {
        advance(*mCurrApplicator);
    }
    scheduleSuccess();
}

void
ApplyBucketsWork::advance(BucketApplicator& applicator)
{
    if (!applicator)
    {
        return;
    }

    assert(mTotalSize != 0);
    mAppliedEntries += applicator.advance();

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
    }

    auto appliedSizeMb = mAppliedSize / 1024 / 1024;
    if (appliedSizeMb > mLastAppliedSizeMb)
    {
        log = true;
        mLastAppliedSizeMb = appliedSizeMb;
    }

    if (log)
    {
        CLOG(INFO, "Bucket")
            << "Bucket-apply: " << mAppliedEntries << " entries in "
            << formatSize(mAppliedSize) << "/" << formatSize(mTotalSize)
            << " in " << mAppliedBuckets << "/" << mTotalBuckets << " files ("
            << (100 * mAppliedSize / mTotalSize) << "%)";
    }
}

Work::State
ApplyBucketsWork::onSuccess()
{
    mApp.getCatchupManager().logAndUpdateCatchupStatus(true);

    if (mSnapApplicator)
    {
        if (*mSnapApplicator)
        {
            return WORK_RUNNING;
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
            return WORK_RUNNING;
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
        CLOG(DEBUG, "History")
            << "ApplyBuckets : starting next level: " << mLevel;
        return WORK_PENDING;
    }

    CLOG(DEBUG, "History") << "ApplyBuckets : done, restarting merges";
    mApp.getBucketManager().assumeState(mApplyState);
    return WORK_SUCCESS;
}

void
ApplyBucketsWork::onFailureRetry()
{
    mBucketApplyFailure.Mark();
    Work::onFailureRetry();
}

void
ApplyBucketsWork::onFailureRaise()
{
    mBucketApplyFailure.Mark();
    Work::onFailureRaise();
}
}
