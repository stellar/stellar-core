// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketUtils.h"
#include "bucket/HotArchiveBucket.h"
#include "bucket/LiveBucket.h"
#include "ledger/LedgerTypeUtils.h"
#include "main/Application.h"
#include "util/types.h"
#include "xdr/Stellar-ledger-entries.h"
#include <fmt/format.h>
#include <medida/counter.h>
#include <medida/metrics_registry.h>

namespace stellar
{

std::string
toString(LedgerEntryTypeAndDurability const type)
{
    switch (type)
    {
    case LedgerEntryTypeAndDurability::ACCOUNT:
        return "ACCOUNT";
    case LedgerEntryTypeAndDurability::TRUSTLINE:
        return "TRUSTLINE";
    case LedgerEntryTypeAndDurability::OFFER:
        return "OFFER";
    case LedgerEntryTypeAndDurability::DATA:
        return "DATA";
    case LedgerEntryTypeAndDurability::CLAIMABLE_BALANCE:
        return "CLAIMABLE_BALANCE";
    case LedgerEntryTypeAndDurability::LIQUIDITY_POOL:
        return "LIQUIDITY_POOL";
    case LedgerEntryTypeAndDurability::TEMPORARY_CONTRACT_DATA:
        return "TEMPORARY_CONTRACT_DATA";
    case LedgerEntryTypeAndDurability::PERSISTENT_CONTRACT_DATA:
        return "PERSISTENT_CONTRACT_DATA";
    case LedgerEntryTypeAndDurability::CONTRACT_CODE:
        return "CONTRACT_CODE";
    case LedgerEntryTypeAndDurability::CONFIG_SETTING:
        return "CONFIG_SETTING";
    case LedgerEntryTypeAndDurability::TTL:
        return "TTL";
    default:
        throw std::runtime_error(
            fmt::format(FMT_STRING("unknown LedgerEntryTypeAndDurability {:d}"),
                        static_cast<uint32_t>(type)));
    }
}

MergeCounters&
MergeCounters::operator+=(MergeCounters const& delta)
{
    mPreInitEntryProtocolMerges += delta.mPreInitEntryProtocolMerges;
    mPostInitEntryProtocolMerges += delta.mPostInitEntryProtocolMerges;

    mRunningMergeReattachments += delta.mRunningMergeReattachments;
    mFinishedMergeReattachments += delta.mFinishedMergeReattachments;

    mPreShadowRemovalProtocolMerges += delta.mPreShadowRemovalProtocolMerges;
    mPostShadowRemovalProtocolMerges += delta.mPostShadowRemovalProtocolMerges;

    mNewMetaEntries += delta.mNewMetaEntries;
    mNewInitEntries += delta.mNewInitEntries;
    mNewLiveEntries += delta.mNewLiveEntries;
    mNewDeadEntries += delta.mNewDeadEntries;
    mOldMetaEntries += delta.mOldMetaEntries;
    mOldInitEntries += delta.mOldInitEntries;
    mOldLiveEntries += delta.mOldLiveEntries;
    mOldDeadEntries += delta.mOldDeadEntries;

    mOldEntriesDefaultAccepted += delta.mOldEntriesDefaultAccepted;
    mNewEntriesDefaultAccepted += delta.mNewEntriesDefaultAccepted;
    mNewInitEntriesMergedWithOldDead += delta.mNewInitEntriesMergedWithOldDead;
    mOldInitEntriesMergedWithNewLive += delta.mOldInitEntriesMergedWithNewLive;
    mOldInitEntriesMergedWithNewDead += delta.mOldInitEntriesMergedWithNewDead;
    mNewEntriesMergedWithOldNeitherInit +=
        delta.mNewEntriesMergedWithOldNeitherInit;

    mShadowScanSteps += delta.mShadowScanSteps;
    mMetaEntryShadowElisions += delta.mMetaEntryShadowElisions;
    mLiveEntryShadowElisions += delta.mLiveEntryShadowElisions;
    mInitEntryShadowElisions += delta.mInitEntryShadowElisions;
    mDeadEntryShadowElisions += delta.mDeadEntryShadowElisions;

    mOutputIteratorTombstoneElisions += delta.mOutputIteratorTombstoneElisions;
    mOutputIteratorBufferUpdates += delta.mOutputIteratorBufferUpdates;
    mOutputIteratorActualWrites += delta.mOutputIteratorActualWrites;
    return *this;
}

bool
MergeCounters::operator==(MergeCounters const& other) const
{
    return (
        mPreInitEntryProtocolMerges == other.mPreInitEntryProtocolMerges &&
        mPostInitEntryProtocolMerges == other.mPostInitEntryProtocolMerges &&

        mRunningMergeReattachments == other.mRunningMergeReattachments &&
        mFinishedMergeReattachments == other.mFinishedMergeReattachments &&

        mNewMetaEntries == other.mNewMetaEntries &&
        mNewInitEntries == other.mNewInitEntries &&
        mNewLiveEntries == other.mNewLiveEntries &&
        mNewDeadEntries == other.mNewDeadEntries &&
        mOldMetaEntries == other.mOldMetaEntries &&
        mOldInitEntries == other.mOldInitEntries &&
        mOldLiveEntries == other.mOldLiveEntries &&
        mOldDeadEntries == other.mOldDeadEntries &&

        mOldEntriesDefaultAccepted == other.mOldEntriesDefaultAccepted &&
        mNewEntriesDefaultAccepted == other.mNewEntriesDefaultAccepted &&
        mNewInitEntriesMergedWithOldDead ==
            other.mNewInitEntriesMergedWithOldDead &&
        mOldInitEntriesMergedWithNewLive ==
            other.mOldInitEntriesMergedWithNewLive &&
        mOldInitEntriesMergedWithNewDead ==
            other.mOldInitEntriesMergedWithNewDead &&
        mNewEntriesMergedWithOldNeitherInit ==
            other.mNewEntriesMergedWithOldNeitherInit &&

        mShadowScanSteps == other.mShadowScanSteps &&
        mMetaEntryShadowElisions == other.mMetaEntryShadowElisions &&
        mLiveEntryShadowElisions == other.mLiveEntryShadowElisions &&
        mInitEntryShadowElisions == other.mInitEntryShadowElisions &&
        mDeadEntryShadowElisions == other.mDeadEntryShadowElisions &&

        mOutputIteratorTombstoneElisions ==
            other.mOutputIteratorTombstoneElisions &&
        mOutputIteratorBufferUpdates == other.mOutputIteratorBufferUpdates &&
        mOutputIteratorActualWrites == other.mOutputIteratorActualWrites);
}

// Check that eviction scan is based off of current ledger snapshot and that
// archival settings have not changed
bool
EvictionResultCandidates::isValid(uint32_t currLedger,
                                  StateArchivalSettings const& currSas) const
{
    return initialLedger == currLedger &&
           initialSas.maxEntriesToArchive == currSas.maxEntriesToArchive &&
           initialSas.evictionScanSize == currSas.evictionScanSize &&
           initialSas.startingEvictionScanLevel ==
               currSas.startingEvictionScanLevel;
}

EvictionCounters::EvictionCounters(Application& app)
    : entriesEvicted(app.getMetrics().NewCounter(
          {"state-archival", "eviction", "entries-evicted"}))
    , bytesScannedForEviction(app.getMetrics().NewCounter(
          {"state-archival", "eviction", "bytes-scanned"}))
    , incompleteBucketScan(app.getMetrics().NewCounter(
          {"state-archival", "eviction", "incomplete-scan"}))
    , evictionCyclePeriod(
          app.getMetrics().NewCounter({"state-archival", "eviction", "period"}))
    , averageEvictedEntryAge(
          app.getMetrics().NewCounter({"state-archival", "eviction", "age"}))
{
}

void
EvictionStatistics::recordEvictedEntry(uint64_t age)
{
    std::lock_guard l(mLock);
    ++mNumEntriesEvicted;
    mEvictedEntriesAgeSum += age;
}

void
EvictionStatistics::submitMetricsAndRestartCycle(uint32_t currLedgerSeq,
                                                 EvictionCounters& counters)
{
    std::lock_guard l(mLock);

    // Only record metrics if we've seen a complete cycle to avoid noise
    if (mCompleteCycle)
    {
        counters.evictionCyclePeriod.set_count(currLedgerSeq -
                                               mEvictionCycleStartLedger);

        auto averageAge = mNumEntriesEvicted == 0
                              ? 0
                              : mEvictedEntriesAgeSum / mNumEntriesEvicted;
        counters.averageEvictedEntryAge.set_count(averageAge);
    }

    // Reset to start new cycle
    mCompleteCycle = true;
    mEvictedEntriesAgeSum = 0;
    mNumEntriesEvicted = 0;
    mEvictionCycleStartLedger = currLedgerSeq;
}

template <>
LedgerEntryTypeAndDurability
bucketEntryToLedgerEntryAndDurabilityType<LiveBucket>(
    LiveBucket::EntryT const& be)
{
    auto bet = be.type();
    LedgerKey key;
    if (bet == INITENTRY || bet == LIVEENTRY)
    {
        key = LedgerEntryKey(be.liveEntry());
    }
    else if (bet == DEADENTRY)
    {
        key = be.deadEntry();
    }
    else
    {
        throw std::runtime_error("Unexpected Bucket Meta Entry");
    }

    switch (key.type())
    {
    case ACCOUNT:
        return LedgerEntryTypeAndDurability::ACCOUNT;
    case TRUSTLINE:
        return LedgerEntryTypeAndDurability::TRUSTLINE;
    case OFFER:
        return LedgerEntryTypeAndDurability::OFFER;
    case DATA:
        return LedgerEntryTypeAndDurability::DATA;
    case CLAIMABLE_BALANCE:
        return LedgerEntryTypeAndDurability::CLAIMABLE_BALANCE;
    case LIQUIDITY_POOL:
        return LedgerEntryTypeAndDurability::LIQUIDITY_POOL;
    case CONTRACT_DATA:
        return isTemporaryEntry(key)
                   ? LedgerEntryTypeAndDurability::TEMPORARY_CONTRACT_DATA
                   : LedgerEntryTypeAndDurability::PERSISTENT_CONTRACT_DATA;
    case CONTRACT_CODE:
        return LedgerEntryTypeAndDurability::CONTRACT_CODE;
    case CONFIG_SETTING:
        return LedgerEntryTypeAndDurability::CONFIG_SETTING;
    case TTL:
        return LedgerEntryTypeAndDurability::TTL;
    default:
        auto label = xdr::xdr_traits<LedgerEntryType>::enum_name(key.type());
        throw std::runtime_error(
            fmt::format("Unknown LedgerEntryType {}", label));
    }
}

template <>
LedgerEntryTypeAndDurability
bucketEntryToLedgerEntryAndDurabilityType<HotArchiveBucket>(
    HotArchiveBucket::EntryT const& be)
{
    auto bet = be.type();
    LedgerKey key;
    if (bet == HOT_ARCHIVE_ARCHIVED)
    {
        key = LedgerEntryKey(be.archivedEntry());
    }
    else if (bet == HOT_ARCHIVE_LIVE || bet == HOT_ARCHIVE_DELETED)
    {
        key = be.key();
    }
    else
    {
        throw std::runtime_error("Unexpected Hot Archive Meta Entry");
    }

    if (key.type() != CONTRACT_DATA && isTemporaryEntry(key))
    {
        throw std::runtime_error("HotArchiveBucketEntry is temporary");
    }

    switch (key.type())
    {
    case CONTRACT_DATA:
        return LedgerEntryTypeAndDurability::PERSISTENT_CONTRACT_DATA;
    case CONTRACT_CODE:
        return LedgerEntryTypeAndDurability::CONTRACT_CODE;
    default:
        auto label = xdr::xdr_traits<LedgerEntryType>::enum_name(key.type());
        throw std::runtime_error(
            fmt::format("Unexpected LedgerEntryType in HotArchive: {}", label));
    }
}

template <>
bool
isBucketMetaEntry<HotArchiveBucket>(HotArchiveBucket::EntryT const& be)
{
    return be.type() == HOT_ARCHIVE_METAENTRY;
}

template <>
bool
isBucketMetaEntry<LiveBucket>(LiveBucket::EntryT const& be)
{
    return be.type() == METAENTRY;
}

template <class BucketT>
void
BucketEntryCounters::count(typename BucketT::EntryT const& be)
{
    if (isBucketMetaEntry<BucketT>(be))
    {
        // Do not count meta entries.
        return;
    }
    auto ledt = bucketEntryToLedgerEntryAndDurabilityType<BucketT>(be);
    entryTypeCounts[ledt]++;
    entryTypeSizes[ledt] += xdr::xdr_size(be);
}

BucketEntryCounters&
BucketEntryCounters::operator+=(BucketEntryCounters const& other)
{
    for (auto [type, count] : other.entryTypeCounts)
    {
        this->entryTypeCounts[type] += count;
    }
    for (auto [type, size] : other.entryTypeSizes)
    {
        this->entryTypeSizes[type] += size;
    }
    return *this;
}

bool
BucketEntryCounters::operator==(BucketEntryCounters const& other) const
{
    return this->entryTypeCounts == other.entryTypeCounts &&
           this->entryTypeSizes == other.entryTypeSizes;
}

bool
BucketEntryCounters::operator!=(BucketEntryCounters const& other) const
{
    return !(*this == other);
}

template void
BucketEntryCounters::count<LiveBucket>(LiveBucket::EntryT const& be);
template void BucketEntryCounters::count<HotArchiveBucket>(
    HotArchiveBucket::EntryT const& be);
}
