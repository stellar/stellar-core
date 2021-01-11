// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "TxSimGenerateBucketsWork.h"
#include "bucket/Bucket.h"
#include "bucket/BucketInputIterator.h"
#include "bucket/BucketList.h"
#include "bucket/BucketManager.h"
#include "bucket/BucketOutputIterator.h"
#include "crypto/SecretKey.h"
#include "crypto/SignerKey.h"
#include "history/HistoryArchive.h"
#include "main/ErrorMessages.h"
#include "src/transactions/simulation/TxSimUtils.h"

namespace stellar
{
namespace txsimulation
{

TxSimGenerateBucketsWork::TxSimGenerateBucketsWork(
    Application& app, std::map<std::string, std::shared_ptr<Bucket>>& buckets,
    HistoryArchiveState const& applyState, uint32_t multiplier)
    : BasicWork(app, "generate-buckets", BasicWork::RETRY_NEVER)
    , mApp(app)
    , mBuckets(buckets)
    , mApplyState(applyState)
    , mMultiplier(multiplier)
    , mLevel(0)
    , mIsCurr(true)
{
}

void
TxSimGenerateBucketsWork::onReset()
{
    mLevel = 0;
    mGeneratedApplyState = {};
    mGeneratedApplyState.currentLedger = mApplyState.currentLedger;
    mPrevSnap.reset();
    mBuckets.clear();
    mIntermediateBuckets.clear();
    mMergesInProgress.clear();
    mIsCurr = true;
}

bool
TxSimGenerateBucketsWork::checkOrStartMerges()
{
    // Resolve all completed merges
    for (auto it = mMergesInProgress.begin(); it != mMergesInProgress.end();)
    {
        if (it->mergeComplete())
        {
            mIntermediateBuckets.emplace_back(it->resolve());
            it = mMergesInProgress.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // Check if we're left with the final bucket, or we haven't
    // started merging yet
    if (mIntermediateBuckets.size() <= 1 && mMergesInProgress.empty())
    {
        return true;
    }

    // Kick-off new merges with newly resolved buckets
    while (mIntermediateBuckets.size() > 1)
    {
        auto b1 = mIntermediateBuckets.front();
        mIntermediateBuckets.pop_front();
        auto b2 = mIntermediateBuckets.front();
        mIntermediateBuckets.pop_front();
        std::vector<std::shared_ptr<Bucket>> shadows;
        mMergesInProgress.emplace_back(mApp, b1, b2, shadows,
                                       Config::CURRENT_LEDGER_PROTOCOL_VERSION,
                                       false, mLevel);
    }

    return false;
}

void
TxSimGenerateBucketsWork::processGeneratedBucket()
{
    // Current bucket has been generated, we're left with the final bucket
    assert(mIntermediateBuckets.size() == 1 && mMergesInProgress.empty());

    auto newBucket = mIntermediateBuckets.front();
    mIntermediateBuckets.pop_front();
    auto hash = newBucket->getHash();
    mBuckets[binToHex(hash)] = newBucket;

    // Forget any intermediate buckets produced
    mApp.getBucketManager().forgetUnreferencedBuckets();

    CLOG_INFO(History, "Generated bucket: {}", hexAbbrev(hash));
    auto& level = mGeneratedApplyState.currentBuckets[mLevel];
    if (mIsCurr)
    {
        level.curr = binToHex(hash);
        setFutureBucket(newBucket);
    }
    else
    {
        level.snap = binToHex(hash);
        mPrevSnap = newBucket;
    }
}

BasicWork::State
TxSimGenerateBucketsWork::onRun()
{
    // Not ready to proceed yet -- still waiting for current bucket generation
    // to finish
    if (!checkOrStartMerges())
    {
        setupWaitingCallback(std::chrono::milliseconds(100));
        return BasicWork::State::WORK_WAITING;
    }

    if (!mIntermediateBuckets.empty())
    {
        processGeneratedBucket();

        if (mLevel < BucketList::kNumLevels - 1)
        {
            if (!mIsCurr)
            {
                ++mLevel;
            }
        }
        else
        {
            try
            {
                // Persist HAS file to avoid re-generating same buckets
                getGeneratedHAS().save("simulate-" +
                                       HistoryArchiveState::baseName());
                return State::WORK_SUCCESS;
            }
            catch (std::exception const& e)
            {
                CLOG_ERROR(History, "Error saving HAS file: {}", e.what());
                CLOG_ERROR(History, "{}", POSSIBLY_CORRUPTED_LOCAL_FS);
                return State::WORK_FAILURE;
            }
        }

        mIsCurr = !mIsCurr;
    }

    try
    {
        auto const& currentLevel = mApplyState.currentBuckets.at(mLevel);
        auto bucketHash = mIsCurr ? currentLevel.curr : currentLevel.snap;
        auto bucket =
            mApp.getBucketManager().getBucketByHash(hexToBin256(bucketHash));
        CLOG_INFO(History, "Simulating {} bucketlist level: {}",
                  (mIsCurr ? "curr" : "snap"), mLevel);
        startBucketGeneration(bucket);
    }
    catch (std::runtime_error const& e)
    {
        CLOG_ERROR(History, "Unable to generate bucket: {}", e.what());
        return BasicWork::State::WORK_FAILURE;
    }

    return BasicWork::State::WORK_RUNNING;
}

void
TxSimGenerateBucketsWork::setFutureBucket(std::shared_ptr<Bucket> const& curr)
{
    auto currLedger = mApplyState.currentLedger;
    mGeneratedApplyState.currentBuckets[mLevel].next.clear();

    if (mPrevSnap && mLevel > 0)
    {
        auto preparedCurr =
            BucketList::shouldMergeWithEmptyCurr(currLedger, mLevel)
                ? std::make_shared<Bucket>()
                : curr;

        auto snapVersion = Bucket::getBucketVersion(mPrevSnap);
        if (snapVersion < Bucket::FIRST_PROTOCOL_SHADOWS_REMOVED)
        {
            // Note: here we use fake empty shadows, since we can't really
            // reconstruct the exact shadows, plus it should not really matter
            // anyway
            std::vector<std::shared_ptr<Bucket>> shadows;
            mGeneratedApplyState.currentBuckets[mLevel].next = FutureBucket(
                mApp, curr, mPrevSnap, shadows, snapVersion, false, mLevel);
        }
    }
}

HistoryArchiveState const&
TxSimGenerateBucketsWork::getGeneratedHAS()
{
    mGeneratedApplyState.resolveAllFutures();
    assert(mGeneratedApplyState.containsValidBuckets(mApp));
    return mGeneratedApplyState;
}

void
TxSimGenerateBucketsWork::startBucketGeneration(
    std::shared_ptr<Bucket> const& oldBucket)
{
    std::vector<LedgerEntry> initEntries, liveEntries;
    std::vector<LedgerKey> deadEntries;
    BucketInputIterator iter(oldBucket);
    uint32_t ledgerVersion = iter.getMetadata().ledgerVersion;

    // Deconstruct the existing bucket to use for simulated bucket generation
    for (; iter; ++iter)
    {
        auto entry = *iter;
        switch (entry.type())
        {
        case METAENTRY:
            break;
        case INITENTRY:
            initEntries.emplace_back(entry.liveEntry());
            break;
        case LIVEENTRY:
            liveEntries.emplace_back(entry.liveEntry());
            break;
        case DEADENTRY:
            deadEntries.emplace_back(entry.deadEntry());
            break;
        }
    }

    // Since the total number of live and dead entries is known, avoid
    // re-allocating vectors inside the loop (std::vector::clear() does not
    // change vector capacity)
    std::vector<LedgerEntry> newInitEntries;
    std::vector<LedgerEntry> newLiveEntries;
    std::vector<LedgerKey> newDeadEntries;

    newInitEntries.reserve(initEntries.size());
    newLiveEntries.reserve(liveEntries.size());
    newDeadEntries.reserve(deadEntries.size());

    mIntermediateBuckets.emplace_back(oldBucket);

    for (uint32_t count = 1; count < mMultiplier; count++)
    {
        generateScaledLiveEntries(newInitEntries, initEntries, count);
        generateScaledLiveEntries(newLiveEntries, liveEntries, count);
        generateScaledDeadEntries(newDeadEntries, deadEntries, count);

        mIntermediateBuckets.emplace_back(
            Bucket::fresh(mApp.getBucketManager(), ledgerVersion,
                          newInitEntries, newLiveEntries, newDeadEntries,
                          /* countMergeEvents */ false,
                          mApp.getClock().getIOContext(), /* doFsync */ false));
    }

    checkOrStartMerges();
}
}
}
