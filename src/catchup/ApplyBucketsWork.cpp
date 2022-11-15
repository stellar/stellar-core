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
    std::map<std::string, std::shared_ptr<Bucket>> const& buckets,
    HistoryArchiveState const& applyState, uint32_t maxProtocolVersion,
    std::function<bool(LedgerEntryType)> onlyApply)
    : Work(app, "apply-buckets", BasicWork::RETRY_NEVER)
    , mBuckets(buckets)
    , mApplyState(applyState)
    , mEntryTypeFilter(onlyApply)
    , mApplying(false)
    , mTotalSize(0)
    , mLevel(BucketList::kNumLevels - 1)
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

std::shared_ptr<Bucket const>
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

    if (!isAborting())
    {
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

    mLevel = BucketList::kNumLevels - 1;
    mApplying = false;
    mDelayChecked = false;
    mSpawnedAssumeStateWork = false;

    mSnapBucket.reset();
    mCurrBucket.reset();
    mSnapApplicator.reset();
    mCurrApplicator.reset();
}

void
ApplyBucketsWork::startLevel()
{
    ZoneScoped;
    releaseAssert(isLevelComplete());

    CLOG_DEBUG(History, "ApplyBuckets : starting level {}", mLevel);
    auto& level = getBucketLevel(mLevel);
    HistoryStateBucket const& i = mApplyState.currentBuckets.at(mLevel);

    bool applySnap = (i.snap != binToHex(level.getSnap()->getHash()));
    bool applyCurr = (i.curr != binToHex(level.getCurr()->getHash()));

    if (mApplying || applySnap)
    {
        mSnapBucket = getBucket(i.snap);
        mMinProtocolVersionSeen = std::min(
            mMinProtocolVersionSeen, Bucket::getBucketVersion(mSnapBucket));
        mSnapApplicator = std::make_unique<BucketApplicator>(
            mApp, mMaxProtocolVersion, mMinProtocolVersionSeen, mLevel,
            mSnapBucket, mEntryTypeFilter);
        CLOG_DEBUG(History, "ApplyBuckets : starting level[{}].snap = {}",
                   mLevel, i.snap);
        mApplying = true;
    }
    if (mApplying || applyCurr)
    {
        mCurrBucket = getBucket(i.curr);
        mMinProtocolVersionSeen = std::min(
            mMinProtocolVersionSeen, Bucket::getBucketVersion(mCurrBucket));
        mCurrApplicator = std::make_unique<BucketApplicator>(
            mApp, mMaxProtocolVersion, mMinProtocolVersionSeen, mLevel,
            mCurrBucket, mEntryTypeFilter);
        CLOG_DEBUG(History, "ApplyBuckets : starting level[{}].curr = {}",
                   mLevel, i.curr);
        mApplying = true;
    }
}

BasicWork::State
ApplyBucketsWork::doWork()
{
    ZoneScoped;

    // Step 1: apply buckets. Step 2: assume state
    if (!mSpawnedAssumeStateWork)
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
        // 1. mCurrApplicator should never be advanced if mSnapApplicator is
        //    not false. Otherwise it is possible for curr to modify the
        //    database when the invariants for snap are checked.
        // 2. There is no reason to advance mSnapApplicator or mCurrApplicator
        //    if there is nothing to be applied.
        if (mSnapApplicator)
        {
            TempLedgerVersionSetter tlvs(mApp, mMaxProtocolVersion);
            if (*mSnapApplicator)
            {
                advance("snap", *mSnapApplicator);
                return State::WORK_RUNNING;
            }
            mApp.getInvariantManager().checkOnBucketApply(
                mSnapBucket, mApplyState.currentLedger, mLevel, false,
                mEntryTypeFilter);
            mSnapApplicator.reset();
            mSnapBucket.reset();
            mApp.getCatchupManager().bucketsApplied();
        }
        if (mCurrApplicator)
        {
            TempLedgerVersionSetter tlvs(mApp, mMaxProtocolVersion);
            if (*mCurrApplicator)
            {
                advance("curr", *mCurrApplicator);
                return State::WORK_RUNNING;
            }
            mApp.getInvariantManager().checkOnBucketApply(
                mCurrBucket, mApplyState.currentLedger, mLevel, true,
                mEntryTypeFilter);
            mCurrApplicator.reset();
            mCurrBucket.reset();
            mApp.getCatchupManager().bucketsApplied();
        }

        if (mLevel != 0)
        {
            --mLevel;
            CLOG_DEBUG(History, "ApplyBuckets : starting next level: {}",
                       mLevel);
            return State::WORK_RUNNING;
        }

        CLOG_INFO(History, "ApplyBuckets : done, assuming state");

        // After all buckets applied, spawn assumeState work
        addWork<AssumeStateWork>(mApplyState, mMaxProtocolVersion);
        mSpawnedAssumeStateWork = true;
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
    return !(mApplying) || !(mSnapApplicator || mCurrApplicator);
}

std::string
ApplyBucketsWork::getStatus() const
{
    if (!mSpawnedAssumeStateWork)
    {
        auto size = mTotalSize == 0 ? 0 : (100 * mAppliedSize / mTotalSize);
        return fmt::format(
            FMT_STRING("Applying buckets {:d}%. Currently on level {:d}"), size,
            mLevel);
    }

    return Work::getStatus();
}
}
