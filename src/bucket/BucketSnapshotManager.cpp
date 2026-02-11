// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketSnapshotManager.h"
#include "bucket/BucketListSnapshot.h"
#include "bucket/HotArchiveBucketList.h"
#include "bucket/LiveBucketList.h"
#include "ledger/LedgerStateSnapshot.h"
#include "main/AppConnector.h"
#include "util/GlobalChecks.h"

namespace stellar
{

BucketSnapshotManager::BucketSnapshotManager(
    AppConnector& app, LiveBucketList const& liveBL,
    HotArchiveBucketList const& hotArchiveBL, LedgerHeader const& header,
    uint32_t numHistoricalLedgers)
    : mAppConnector(app)
    , mCurrLiveSnapshot(
          std::make_shared<BucketListSnapshotData<LiveBucket>>(liveBL))
    , mCurrHotArchiveSnapshot(
          std::make_shared<BucketListSnapshotData<HotArchiveBucket>>(
              hotArchiveBL))
    , mCurrHeader(header)
    , mNumHistoricalSnapshots(numHistoricalLedgers)
{
    releaseAssert(threadIsMain());
    releaseAssert(mCurrLiveSnapshot);
    releaseAssert(mCurrHotArchiveSnapshot);

    // Initialize mCompleteState so that copyLedgerStateSnapshot() is valid
    // even before the first ledger close calls setCompleteState().
    // TODO: make less sketchy
    LedgerHeaderHistoryEntry lcl;
    lcl.header = header;
    HistoryArchiveState has;
    has.currentLedger = header.ledgerSeq;
    mCompleteState = std::shared_ptr<CompleteConstLedgerState const>(
        new CompleteConstLedgerState(
            mCurrLiveSnapshot, mLiveHistoricalSnapshots,
            mCurrHotArchiveSnapshot, mHotArchiveHistoricalSnapshots, lcl, has));
}

std::map<uint32_t, std::shared_ptr<BucketListSnapshotData<LiveBucket> const>>
BucketSnapshotManager::getLiveHistoricalSnapshots() const
{
    SharedLockShared guard(mSnapshotMutex);
    return mLiveHistoricalSnapshots;
}

std::map<uint32_t,
         std::shared_ptr<BucketListSnapshotData<HotArchiveBucket> const>>
BucketSnapshotManager::getHotArchiveHistoricalSnapshots() const
{
    SharedLockShared guard(mSnapshotMutex);
    return mHotArchiveHistoricalSnapshots;
}

void
BucketSnapshotManager::updateCurrentSnapshot(
    LiveBucketList const& liveBL, HotArchiveBucketList const& hotArchiveBL,
    LedgerHeader const& header)
{
    auto updateSnapshot = [numHistoricalSnapshots = mNumHistoricalSnapshots](
                              auto& currentSnapshot, auto& historicalSnapshots,
                              auto newSnapshot, uint32_t currLedgerSeq) {
        releaseAssert(newSnapshot);

        // First update historical snapshots
        if (numHistoricalSnapshots != 0 && currentSnapshot)
        {
            // If historical snapshots are full, delete the oldest one
            if (historicalSnapshots.size() == numHistoricalSnapshots)
            {
                historicalSnapshots.erase(historicalSnapshots.begin());
            }

            historicalSnapshots.emplace(currLedgerSeq,
                                        std::move(currentSnapshot));
        }

        currentSnapshot = std::move(newSnapshot);
    };

    auto newLiveSnapshot =
        std::make_shared<BucketListSnapshotData<LiveBucket>>(liveBL);
    auto newHotArchiveSnapshot =
        std::make_shared<BucketListSnapshotData<HotArchiveBucket>>(
            hotArchiveBL);

    // Updating canonical snapshots requires exclusive write access
    SharedLockExclusive guard(mSnapshotMutex);
    releaseAssert(header.ledgerSeq >= mCurrHeader.ledgerSeq);
    updateSnapshot(mCurrLiveSnapshot, mLiveHistoricalSnapshots,
                   std::move(newLiveSnapshot), mCurrHeader.ledgerSeq);
    updateSnapshot(mCurrHotArchiveSnapshot, mHotArchiveHistoricalSnapshots,
                   std::move(newHotArchiveSnapshot), mCurrHeader.ledgerSeq);
    mCurrHeader = header;

    // Rebuild mCompleteState from the updated raw data so that
    // copyLedgerStateSnapshot() always returns fresh state. This uses the
    // private constructor that skips Soroban config loading; the normal
    // ledger-close path will follow up with setCompleteState() which
    // provides the full state including Soroban config.
    // TODO: make less sketchy
    LedgerHeaderHistoryEntry lcl;
    lcl.header = header;
    HistoryArchiveState has;
    has.currentLedger = header.ledgerSeq;
    mCompleteState = std::shared_ptr<CompleteConstLedgerState const>(
        new CompleteConstLedgerState(
            mCurrLiveSnapshot, mLiveHistoricalSnapshots,
            mCurrHotArchiveSnapshot, mHotArchiveHistoricalSnapshots, lcl, has));
}

void
BucketSnapshotManager::setCompleteState(CompleteConstLedgerStatePtr state)
{
    SharedLockExclusive guard(mSnapshotMutex);
    mCompleteState = std::move(state);
}

LedgerStateSnapshot
BucketSnapshotManager::copyLedgerStateSnapshot() const
{
    SharedLockShared guard(mSnapshotMutex);
    releaseAssert(mCompleteState);
    return LedgerStateSnapshot(mCompleteState, mAppConnector.getMetrics());
}

void
BucketSnapshotManager::maybeUpdateLedgerStateSnapshot(
    LedgerStateSnapshot& snapshot) const
{
    SharedLockShared guard(mSnapshotMutex);
    releaseAssert(mCompleteState);
    if (snapshot.getLedgerSeq() !=
        mCompleteState->getLastClosedLedgerHeader().header.ledgerSeq)
    {
        snapshot =
            LedgerStateSnapshot(mCompleteState, mAppConnector.getMetrics());
    }
}
}
