// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/ApplyBucketsWork.h"
#include "bucket/Bucket.h"
#include "bucket/BucketApplicator.h"
#include "bucket/BucketList.h"
#include "bucket/BucketManager.h"
#include "crypto/Hex.h"
#include "crypto/SecretKey.h"
#include "history/HistoryArchive.h"
#include "historywork/Progress.h"
#include "invariant/InvariantManager.h"
#include "ledger/AccountFrame.h"
#include "ledger/DataFrame.h"
#include "ledger/LedgerManager.h"
#include "ledger/OfferFrame.h"
#include "ledger/TrustFrame.h"
#include "main/Application.h"
#include "util/format.h"
#include "util/make_unique.h"
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
        AccountFrame::deleteAccountsModifiedOnOrAfterLedger(mApp.getDatabase(),
                                                            oldestLedger);
        TrustFrame::deleteTrustLinesModifiedOnOrAfterLedger(mApp.getDatabase(),
                                                            oldestLedger);
        OfferFrame::deleteOffersModifiedOnOrAfterLedger(mApp.getDatabase(),
                                                        oldestLedger);
        DataFrame::deleteDataModifiedOnOrAfterLedger(mApp.getDatabase(),
                                                     oldestLedger);
    }

    if (mApplying || applySnap)
    {
        mSnapBucket = getBucket(i.snap);
        mSnapApplicator =
            make_unique<BucketApplicator>(mApp.getDatabase(), mSnapBucket);
        CLOG(DEBUG, "History") << "ApplyBuckets : starting level[" << mLevel
                               << "].snap = " << i.snap;
        mApplying = true;
        mBucketApplyStart.Mark();
    }
    if (mApplying || applyCurr)
    {
        mCurrBucket = getBucket(i.curr);
        mCurrApplicator =
            make_unique<BucketApplicator>(mApp.getDatabase(), mCurrBucket);
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
        if (*mSnapApplicator)
        {
            mSnapApplicator->advance();
        }
    }
    else if (mCurrApplicator)
    {
        if (*mCurrApplicator)
        {
            mCurrApplicator->advance();
        }
    }
    scheduleSuccess();
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
