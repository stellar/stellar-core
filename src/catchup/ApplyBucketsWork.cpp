// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/ApplyBucketsWork.h"
#include "bucket/Bucket.h"
#include "bucket/BucketApplicator.h"
#include "bucket/BucketList.h"
#include "bucket/BucketManager.h"
#include "catchup/AssumeStateWork.h"
#include "catchup/CatchupManager.h"
#include "catchup/IndexBucketsWork.h"
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

uint32_t
ApplyBucketsWork::startingLevel()
{
    return mApp.getConfig().isUsingBucketListDB() ? 0
                                                  : BucketList::kNumLevels - 1;
}

bool
ApplyBucketsWork::appliedAllLevels() const
{
    if (mApp.getConfig().isUsingBucketListDB())
    {
        return mLevel == BucketList::kNumLevels - 1;
    }
    else
    {
        return mLevel == 0;
    }
}

uint32_t
ApplyBucketsWork::nextLevel() const
{
    return mApp.getConfig().isUsingBucketListDB() ? mLevel + 1 : mLevel - 1;
}

ApplyBucketsWork::ApplyBucketsWork(
    Application& app,
    std::map<std::string, std::shared_ptr<Bucket>> const& buckets,
    HistoryArchiveState const& applyState, uint32_t maxProtocolVersion,
    std::function<bool(LedgerEntryType)> onlyApply)
    : Work(app, "apply-buckets", BasicWork::RETRY_NEVER)
    , mBuckets(buckets)
    , mApplyState(applyState)
    , mEntryTypeFilter(onlyApply)
    , mApplying(false)
    , mTotalSize(0)
    , mLevel(startingLevel())
    , mMaxProtocolVersion(maxProtocolVersion)
    , mCounters(app.getClock().now())
{
}

ApplyBucketsWork::ApplyBucketsWork(
    Application& app,
    std::map<std::string, std::shared_ptr<Bucket>> const& buckets,
    HistoryArchiveState const& applyState, uint32_t maxProtocolVersion)
    : ApplyBucketsWork(app, buckets, applyState, maxProtocolVersion,
                       [](LedgerEntryType) { return true; })
{
}

BucketLevel&
ApplyBucketsWork::getBucketLevel(uint32_t level)
{
    return mApp.getBucketManager().getBucketList().getLevel(level);
}

std::shared_ptr<Bucket>
ApplyBucketsWork::getBucket(std::string const& hash)
{
    auto i = mBuckets.find(hash);
    auto b = (i != mBuckets.end())
                 ? i->second
                 : mApp.getBucketManager().getBucketByHash(hexToBin256(hash));
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
    mMinProtocolVersionSeen = UINT32_MAX;
    mSeenKeys.clear();
    mBucketsToIndex.clear();

    if (!isAborting())
    {
        if (mApp.getConfig().isUsingBucketListDB())
        {
            // The current size of this set is 1.6 million during BucketApply
            // (as of 12/20/23). There's not a great way to estimate this, so
            // reserving with some extra wiggle room
            mSeenKeys.reserve(2'000'000);
        }

        // When applying buckets with accounts, we have to make sure that the
        // root account has been removed. This comes into play, for example,
        // when applying buckets from genesis the root account already exists.
        if (mEntryTypeFilter(ACCOUNT))
        {
            TempLedgerVersionSetter tlvs(mApp, mMaxProtocolVersion);
            {
                SecretKey skey = SecretKey::fromSeed(mApp.getNetworkID());

                LedgerTxn ltx(mApp.getLedgerTxnRoot());
                auto rootAcc = loadAccount(ltx, skey.getPublicKey());
                if (rootAcc)
                {
                    rootAcc.erase();
                }
                ltx.commit();
            }
        }

        auto addBucket = [this](std::shared_ptr<Bucket> const& bucket) {
            if (bucket->getSize() > 0)
            {
                mTotalBuckets++;
                mTotalSize += bucket->getSize();
            }

            if (mApp.getConfig().isUsingBucketListDB())
            {
                mBucketsToIndex.emplace_back(bucket);
            }
        };

        for (auto const& hsb : mApplyState.currentBuckets)
        {
            addBucket(getBucket(hsb.snap));
            addBucket(getBucket(hsb.curr));
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

    mLevel = startingLevel();
    mApplying = false;
    mDelayChecked = false;

    mIndexBucketsWork.reset();
    mAssumeStateWork.reset();

    mFirstBucket.reset();
    mSecondBucket.reset();
    mFirstBucketApplicator.reset();
    mSecondBucketApplicator.reset();
}

// We iterate through the BucketList either in-order (level 0 curr, level 0
// snap, level 1 curr, etc) when only applying offers, or in reverse order
// (level 9 curr, level 8 snap, level 8 curr, etc) when applying all entry
// types. When only applying offers, we keep track of the keys we have already
// seen, and only apply an entry to the DB if it has not been seen before. This
// allows us to perform a single write to the DB and ensure that only the newest
// version is written.
//
// When applying all entry types, this seen keys set would be too large. Since
// there can be no seen keys set, if we were to apply every entry in order, we
// would overwrite the newest version of an entry with an older version as we
// iterate through the BucketList. Due to this, we iterate in reverse order such
// that the newest version of a key is written last, overwriting the older
// versions. This is much slower due to DB churn.
void
ApplyBucketsWork::startLevel()
{
    ZoneScoped;
    releaseAssert(isLevelComplete());

    CLOG_DEBUG(History, "ApplyBuckets : starting level {}", mLevel);
    auto& level = getBucketLevel(mLevel);
    HistoryStateBucket const& i = mApplyState.currentBuckets.at(mLevel);
    bool isUsingBucketListDB = mApp.getConfig().isUsingBucketListDB();

    bool applyFirst = isUsingBucketListDB
                          ? (i.curr != binToHex(level.getCurr()->getHash()))
                          : (i.snap != binToHex(level.getSnap()->getHash()));
    bool applySecond = isUsingBucketListDB
                           ? (i.snap != binToHex(level.getSnap()->getHash()))
                           : (i.curr != binToHex(level.getCurr()->getHash()));

    if (mApplying || applyFirst)
    {
        mFirstBucket = getBucket(isUsingBucketListDB ? i.curr : i.snap);
        mMinProtocolVersionSeen = std::min(
            mMinProtocolVersionSeen, Bucket::getBucketVersion(mFirstBucket));
        mFirstBucketApplicator = std::make_unique<BucketApplicator>(
            mApp, mMaxProtocolVersion, mMinProtocolVersionSeen, mLevel,
            mFirstBucket, mEntryTypeFilter, mSeenKeys);

        if (isUsingBucketListDB)
        {
            CLOG_DEBUG(History, "ApplyBuckets : starting level[{}].curr = {}",
                       mLevel, i.curr);
        }
        else
        {
            CLOG_DEBUG(History, "ApplyBuckets : starting level[{}].snap = {}",
                       mLevel, i.snap);
        }

        mApplying = true;
    }
    if (mApplying || applySecond)
    {
        mSecondBucket = getBucket(isUsingBucketListDB ? i.snap : i.curr);
        mMinProtocolVersionSeen = std::min(
            mMinProtocolVersionSeen, Bucket::getBucketVersion(mSecondBucket));
        mSecondBucketApplicator = std::make_unique<BucketApplicator>(
            mApp, mMaxProtocolVersion, mMinProtocolVersionSeen, mLevel,
            mSecondBucket, mEntryTypeFilter, mSeenKeys);

        if (isUsingBucketListDB)
        {
            CLOG_DEBUG(History, "ApplyBuckets : starting level[{}].snap = {}",
                       mLevel, i.snap);
        }
        else
        {
            CLOG_DEBUG(History, "ApplyBuckets : starting level[{}].curr = {}",
                       mLevel, i.curr);
        }
        mApplying = true;
    }
}

BasicWork::State
ApplyBucketsWork::doWork()
{
    ZoneScoped;

    // Step 1: index buckets. Step 2: apply buckets. Step 3: assume state
    bool isUsingBucketListDB = mApp.getConfig().isUsingBucketListDB();
    if (isUsingBucketListDB)
    {
        if (!mIndexBucketsWork)
        {
            // Spawn indexing work for the first time
            mIndexBucketsWork = addWork<IndexBucketsWork>(mBucketsToIndex);
            return State::WORK_RUNNING;
        }
        else if (mIndexBucketsWork->getState() !=
                 BasicWork::State::WORK_SUCCESS)
        {
            // Exit early if indexing work is still running, or failed
            return mIndexBucketsWork->getState();
        }

        // Otherwise, continue with next steps
    }

    if (!mAssumeStateWork)
    {
        if (mApp.getLedgerManager().rebuildingInMemoryState() && !mDelayChecked)
        {
            mDelayChecked = true;
            auto delay = mApp.getConfig()
                             .ARTIFICIALLY_DELAY_BUCKET_APPLICATION_FOR_TESTING;
            if (delay != std::chrono::seconds::zero())
            {
                CLOG_INFO(History, "Delay bucket application by {} seconds",
                          delay.count());
                setupWaitingCallback(delay);
                return State::WORK_WAITING;
            }
        }

        // Check if we're at the beginning of the new level
        if (isLevelComplete())
        {
            startLevel();
        }

        // The structure of these if statements is motivated by the following:
        // 1. mSecondBucketApplicator should never be advanced if
        //    mFirstBucketApplicator is not false. Otherwise it is possible for
        //    second bucket to modify the database when the invariants for first
        //    bucket are checked.
        // 2. There is no reason to advance mFirstBucketApplicator or
        //    mSecondBucketApplicator  if there is nothing to be applied.
        if (mFirstBucketApplicator)
        {
            TempLedgerVersionSetter tlvs(mApp, mMaxProtocolVersion);

            // When BucketListDB is enabled, we apply in order starting with
            // curr. If BucketListDB is not enabled, we iterate in reverse
            // starting with snap.
            bool isCurr = isUsingBucketListDB;
            if (*mFirstBucketApplicator)
            {
                advance(isCurr ? "curr" : "snap", *mFirstBucketApplicator);
                return State::WORK_RUNNING;
            }
            mApp.getInvariantManager().checkOnBucketApply(
                mFirstBucket, mApplyState.currentLedger, mLevel, isCurr,
                mEntryTypeFilter);
            mFirstBucketApplicator.reset();
            mFirstBucket.reset();
            mApp.getCatchupManager().bucketsApplied();
        }
        if (mSecondBucketApplicator)
        {
            bool isCurr = !isUsingBucketListDB;
            TempLedgerVersionSetter tlvs(mApp, mMaxProtocolVersion);
            if (*mSecondBucketApplicator)
            {
                advance(isCurr ? "curr" : "snap", *mSecondBucketApplicator);
                return State::WORK_RUNNING;
            }
            mApp.getInvariantManager().checkOnBucketApply(
                mSecondBucket, mApplyState.currentLedger, mLevel, isCurr,
                mEntryTypeFilter);
            mSecondBucketApplicator.reset();
            mSecondBucket.reset();
            mApp.getCatchupManager().bucketsApplied();
        }

        if (!appliedAllLevels())
        {
            mLevel = nextLevel();
            CLOG_DEBUG(History, "ApplyBuckets : starting next level: {}",
                       mLevel);
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

bool
ApplyBucketsWork::isLevelComplete()
{
    return !(mApplying) || !(mFirstBucketApplicator || mSecondBucketApplicator);
}

std::string
ApplyBucketsWork::getStatus() const
{
    // This status string only applies to step 2 when we actually apply the
    // buckets.
    bool doneIndexing = !mApp.getConfig().isUsingBucketListDB() ||
                        (mIndexBucketsWork && mIndexBucketsWork->isDone());
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
