// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/ParallelApplyUtils.h"
#include "bucket/BucketSnapshotManager.h"
#include "bucket/BucketUtils.h"
#include "ledger/LedgerTxn.h"
#include "ledger/NetworkConfig.h"
#include "main/AppConnector.h"
#include "transactions/MutableTransactionResult.h"
#include "transactions/TransactionFrameBase.h"
#include "util/GlobalChecks.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdrpp/printer.h"
#include <fmt/core.h>
#include <fmt/std.h>
#include <thread>

namespace
{
using namespace stellar;

// Notes on parallelism and TTL bumps
// ==================================
//
// We say two soroban txs "conflict" if the RW footprint of either tx intersects
// with the RO _or_ RW footprints of the other. Put another way: if either might
// be able to observe whether it ran before or after the other.
//
// The `ParallelTxSetBuilder` partitions a txset into stages and each stage into
// _clusters_ such that there are no conflicts between the clusters of a stage.
// Within a cluster, any two txs may or may not conflict. But between clusters
// they definitely do not.
//
//
// Read-only TTL bumps
// -------------------
//
// We special-case one action that we expect to be quite common: when a tx bumps
// the TTL of an LE that is otherwise _not written_ by the tx. For example
// bumping the TTL of a popular contract instance when executing it. We call
// this action `RoTTLBump(LE)`, and it is treated as a pseudo-write that can
// potentially commute with all other RoTTLBump(LE) actions. Specifically it
// causes LE to only go in the tx's RO footprint, not its RW footprint.
//
// This is enough to cause the following:
//
//   - If no txs in a stage do write(LE), the RoTTLBump(LE)-containing txs are
//     free to run in parallel, do not effect clustering. We merge the bumps
//     performed by each cluster using std::max() when committing it back to the
//     global state (see GlobalParallelApplyLedgerState::maybeMergeRoTTLBumps)
//
//   - If _any_ tx in a stage does write(LE) it will have LE in its RW
//     footprint, and so conflict with all txs doing RoTTLBump(LE). All of them
//     will get clustered together, no bumps can happen in parallel. This is
//     correct since the order of bumping and writing is observable both ways:
//
//       1. An RoTTLBump(LE) will cost a different fee if it happens before or
//          after write(LE) since the write(LE) can change LE's size.
//
//       2. a write(LE) will cost a different fee if it happens before or after
//          an RoTTLBump(LE) since the RoTTLBump(LE) can change LE's TTL.
//
//
// Deferred read-only TTL bumps
// ----------------------------
//
// We want to retain the ability for future versions of stellar-core to run txs
// within a cluster in "as much parallelism as is legal", by further analyzing
// the conflict relationships that exist _inside_ each cluster and scheduling
// non-conflicting txs in parallel. But any given write(LE) in a cluster
// essentially represents a synchronization barrier for all RoTTLBump(LE)
// operations: those RoTTLBump(LE)s that run before the write(LE) don't conflict
// with one another, but they _do_ conflict with the write(LE) and so a future
// scheduler will have to commit to at least a partial order between _groups_ of
// RoTTLBump(LE)s and individual write(LE)s.
//
// In absence of such a fancy future scheduler, we run each cluster in
// sequential order, using the (somewhat incidental) total order that the
// cluster is given to us as "the schedule", and we do our best not to constrain
// future stellar-cores to replay in exactly this order. Specifically we defer
// the effects of each RoTTLBump(LE) by merging them into a separate map
// (mRoTTLBumps) that we only flush back to the ledger at each write(LE), as
// well as the end of the cluster. This bakes-in to the history the execution
// order of groups of RoTTLBump(LE)s and write(LE)s -- as it must! -- but not
// the order of execution within each group of RoTTLBump(LE)s before or after
// each write(LE). In other words we wind up constraining the future scheduler
// by a partial order, not the total order.
//
// Note: by deferring the visibility of RoTTLBump(LE) effects this way it is
// possible that slightly higher fees are charged. For example if we had
// transactions A, B and C in the total order and A and B both do the same
// RoTTLBump(LE) then C does write(LE), A's bump will be deferred until C, and
// so B will pay to do the same bump again. Whereas if we were to commit to a
// total order, B could save this fee, but we would lose the ability to run A
// and B in parallel in the future. CAP 0063 explicitly chose this tradeoff.

std::unordered_set<LedgerKey>
getReadWriteKeysForStage(ApplyStage const& stage)
{
    std::unordered_set<LedgerKey> res;

    for (auto const& txBundle : stage)
    {
        for (auto const& lk :
             txBundle.getTx()->sorobanResources().footprint.readWrite)
        {
            res.emplace(lk);
            if (isSorobanEntry(lk))
            {
                res.emplace(getTTLKey(lk));
            }
        }
    }
    return res;
}

bool
maybeMergeRoTTLBumps(LedgerKey const& key, ParallelApplyEntry const& newEntry,
                     ParallelApplyEntry& oldEntry,
                     std::unordered_set<LedgerKey> const& readWriteSet)
{
    // Read Only bumps will always be updating a pre-existing value. TTL
    // creation (!oldEntry) or deletion (!newEntry) are write conflicts that
    // don't have merge special casing.
    if (newEntry.mLedgerEntry && oldEntry.mLedgerEntry && key.type() == TTL)
    {
        auto const& newLe = newEntry.mLedgerEntry.value();
        auto& oldLe = oldEntry.mLedgerEntry.value();

        releaseAssertOrThrow(newLe.data.type() == TTL);
        releaseAssertOrThrow(oldLe.data.type() == TTL);
        if (readWriteSet.find(key) == readWriteSet.end())
        {
            auto const& newTTL = newLe.data.ttl().liveUntilLedgerSeq;
            auto& oldTTL = oldLe.data.ttl().liveUntilLedgerSeq;
            oldTTL = std::max(oldTTL, newTTL);
            return true;
        }
    }
    return false;
}

void
preParallelApplyAndCollectModifiedClassicEntries(
    AppConnector& app, AbstractLedgerTxn& ltx,
    std::vector<ApplyStage> const& stages,
    ParallelApplyEntryMap& globalEntryMap,
    SorobanNetworkConfig const& sorobanConfig)
{
    releaseAssert(threadIsMain() ||
                  app.threadIsType(Application::ThreadType::APPLY));

    auto fetchInMemoryClassicEntries =
        [&](xdr::xvector<LedgerKey> const& keys) {
            for (auto const& lk : keys)
            {
                if (isSorobanEntry(lk))
                {
                    continue;
                }

                auto entryPair = ltx.getNewestVersionBelowRoot(lk);
                if (!entryPair.first)
                {
                    continue;
                }

                std::optional<LedgerEntry> entry =
                    entryPair.second ? std::make_optional<LedgerEntry>(
                                           entryPair.second->ledgerEntry())
                                     : std::nullopt;

                globalEntryMap.emplace(lk, ParallelApplyEntry{entry, false});
            }
        };

    // First call preParallelApply on all transactions,
    // and then load from footprints. This order is important
    // because preParallelApply modifies the fee source accounts
    // and those accounts could show up in the footprint
    // of a different transaction.
    for (auto const& stage : stages)
    {
        for (auto const& txBundle : stage)
        {
            // Make sure to call preParallelApply on all txs because this will
            // modify the fee source accounts sequence numbers.
            txBundle.getTx()->preParallelApply(
                app, ltx, txBundle.getEffects().getMeta(),
                txBundle.getResPayload(), sorobanConfig);
        }
    }

    for (auto const& stage : stages)
    {
        for (auto const& txBundle : stage)
        {
            auto const& footprint =
                txBundle.getTx()->sorobanResources().footprint;

            fetchInMemoryClassicEntries(footprint.readWrite);
            fetchInMemoryClassicEntries(footprint.readOnly);
        }
    }
}

inline uint32_t&
ttl(LedgerEntry& le)
{
    return le.data.ttl().liveUntilLedgerSeq;
}

inline uint32_t const&
ttl(LedgerEntry const& le)
{
    return le.data.ttl().liveUntilLedgerSeq;
}

inline uint32_t&
ttl(std::optional<LedgerEntry>& le)
{
    return ttl(le.value());
}

inline uint32_t const&
ttl(std::optional<LedgerEntry> const& le)
{
    return ttl(le.value());
}

// Construct a set of all the TTL keys associated with all RO soroban
// (code-or-data) keys named in the footprint of the `txBundle`. Note
// that since RO and RW footprints are disjoint, we only have to look
// at the RO set.
UnorderedSet<LedgerKey>
buildRoTTLSet(TxBundle const& txBundle)
{
    UnorderedSet<LedgerKey> isReadOnlyTTLSet;
    for (auto const& ro :
         txBundle.getTx()->sorobanResources().footprint.readOnly)
    {
        if (!isSorobanEntry(ro))
        {
            continue;
        }
        isReadOnlyTTLSet.emplace(getTTLKey(ro));
    }
    return isReadOnlyTTLSet;
}

// Accumulate into the buffer of `roTTLBumps` the max of any existing entry and
// the provided `updatedLE`, which must be a non-nullopt TTL LE.
void
updateMaxOfRoTTLBump(UnorderedMap<LedgerKey, uint32_t>& roTTLBumps,
                     LedgerKey const& lk,
                     std::optional<LedgerEntry> const& updatedLe)
{
    auto [it, emplaced] = roTTLBumps.emplace(lk, ttl(updatedLe));
    if (!emplaced)
    {
        it->second = std::max(it->second, ttl(updatedLe));
    }
}

}

namespace stellar
{

PreV23LedgerAccessHelper::PreV23LedgerAccessHelper(AbstractLedgerTxn& ltx)
    : mLtx(ltx)
{
}

std::optional<LedgerEntry>
PreV23LedgerAccessHelper::getLedgerEntryOpt(LedgerKey const& key)
{
    auto ltxe = mLtx.loadWithoutRecord(key);
    if (ltxe)
    {
        return ltxe.current();
    }
    return std::nullopt;
}

uint32_t
PreV23LedgerAccessHelper::getLedgerVersion()
{
    return mLtx.loadHeader().current().ledgerVersion;
}

uint32_t
PreV23LedgerAccessHelper::getLedgerSeq()
{
    return mLtx.loadHeader().current().ledgerSeq;
}

bool
PreV23LedgerAccessHelper::upsertLedgerEntry(LedgerKey const& key,
                                            LedgerEntry const& entry)
{
    auto ltxe = mLtx.load(key);
    if (ltxe)
    {
        ltxe.current() = entry;
        return false;
    }
    else
    {
        mLtx.create(entry);
        return true;
    }
}

bool
PreV23LedgerAccessHelper::eraseLedgerEntryIfExists(LedgerKey const& key)
{
    auto ltxe = mLtx.load(key);
    if (ltxe)
    {
        mLtx.erase(key);
        return true;
    }
    return false;
}

ParallelLedgerAccessHelper::ParallelLedgerAccessHelper(
    ThreadParallelApplyLedgerState const& threadState,
    ParallelLedgerInfo const& ledgerInfo)
    : mLedgerInfo(ledgerInfo), mOpState(threadState)
{
    releaseAssertOrThrow(ledgerInfo.getLedgerSeq() ==
                         threadState.getSnapshotLedgerSeq() + 1);
}

std::optional<LedgerEntry>
ParallelLedgerAccessHelper::getLedgerEntryOpt(LedgerKey const& key)
{
    return mOpState.getLiveEntryOpt(key);
}

uint32_t
ParallelLedgerAccessHelper::getLedgerSeq()
{
    auto applySeq = mLedgerInfo.getLedgerSeq();
    releaseAssertOrThrow(applySeq == mOpState.getSnapshotLedgerSeq() + 1);
    return applySeq;
}

uint32_t
ParallelLedgerAccessHelper::getLedgerVersion()
{
    return mLedgerInfo.getLedgerVersion();
}

bool
ParallelLedgerAccessHelper::upsertLedgerEntry(LedgerKey const& key,
                                              LedgerEntry const& entry)
{
    return mOpState.upsertEntry(key, entry, mLedgerInfo.getLedgerSeq());
}

bool
ParallelLedgerAccessHelper::eraseLedgerEntryIfExists(LedgerKey const& key)
{
    return mOpState.eraseEntryIfExists(key);
}

// We model the work-in-progress state of a ledger during parallel application
// in terms of a set of maps and snapshots. The relationships are subtle but
// basically follow an "newer information overrides older" pattern: per-op maps
// override per-thread maps which override the cross-thread "global" maps which
// override the bucket list snapshots. And of course when each newer type is
// successful it commits to its parent / older type.
//
// In this way the structure mirrors the ltx, but is not generalized to
// arbitrary numbers of parent/child levels and, crucially, has some special
// rules around _threading_. The per-thread objects retain no references at all
// to the global maps or snapshots, which are not threadsafe. Instead all
// information the per-thread maps will need is copied into the them when
// they're built, and only committed back to the parent once the threads using
// them are complete.
class ThreadParalllelApplyLedgerState;
GlobalParallelApplyLedgerState::GlobalParallelApplyLedgerState(
    AppConnector& app, AbstractLedgerTxn& ltx,
    std::vector<ApplyStage> const& stages,
    InMemorySorobanState const& inMemoryState,
    SorobanNetworkConfig const& sorobanConfig)
    : mHotArchiveSnapshot(app.copySearchableHotArchiveBucketListSnapshot())
    , mLiveSnapshot(app.copySearchableLiveBucketListSnapshot())
    , mInMemorySorobanState(inMemoryState)
    , mSorobanConfig(sorobanConfig)
{
    releaseAssertOrThrow(ltx.getHeader().ledgerSeq ==
                         getSnapshotLedgerSeq() + 1);

    // From now on, we will be using globalState, liveSnapshots, and the
    // hotArchive to collect all entries. Before we continue though, we need to
    // load into the globalEntryMap any classic entries that have been modified
    // in this ledger because those changes won't be reflected in the
    // globalEntryMap. The entries that could've changed are accounts and
    // trustlines from the classic phase, as well as fee source accounts that
    // had their sequence numbers bumped and fees charged. preParallelApply will
    // update sequence numbers so it needs to be called before we check
    // LedgerTxn.
    preParallelApplyAndCollectModifiedClassicEntries(
        app, ltx, stages, mGlobalEntryMap, mSorobanConfig);
}

void
GlobalParallelApplyLedgerState::commitChangesToLedgerTxn(
    AbstractLedgerTxn& ltx) const
{
    LedgerTxn ltxInner(ltx);
    for (auto const& entry : mGlobalEntryMap)
    {
        // Only update if dirty bit is set
        if (!entry.second.mIsDirty)
        {
            continue;
        }

        if (entry.second.mLedgerEntry)
        {
            auto const& updatedEntry = *entry.second.mLedgerEntry;
            auto ltxe = ltxInner.load(entry.first);
            if (ltxe)
            {
                ltxe.current() = updatedEntry;
            }
            else
            {
                ltxInner.create(updatedEntry);
            }
        }
        else
        {
            auto ltxe = ltxInner.load(entry.first);
            if (ltxe)
            {
                ltxInner.erase(entry.first);
            }
        }
    }

    // While the final state of a restored key that will be written to the
    // Live BucketList is already handled in mGlobalEntryMap, we need to
    // let the ltx know what keys need to be removed from the Hot Archive.
    for (auto const& kvp : mGlobalRestoredEntries.hotArchive)
    {
        // We will search for the ttl key in the hot archive when the entry
        // is seen
        if (kvp.first.type() != TTL)
        {
            auto it =
                mGlobalRestoredEntries.hotArchive.find(getTTLKey(kvp.first));
            releaseAssertOrThrow(it != mGlobalRestoredEntries.hotArchive.end());
            ltxInner.markRestoredFromHotArchive(kvp.second, it->second);
        }
    }
    ltxInner.commit();
}

uint32_t
GlobalParallelApplyLedgerState::getSnapshotLedgerSeq() const
{
    releaseAssertOrThrow(mLiveSnapshot->getLedgerSeq() ==
                         mHotArchiveSnapshot->getLedgerSeq());
    releaseAssertOrThrow(mLiveSnapshot->getLedgerSeq() ==
                         mInMemorySorobanState.getLedgerSeq());
    return mLiveSnapshot->getLedgerSeq();
}

ParallelApplyEntryMap const&
GlobalParallelApplyLedgerState::getGlobalEntryMap() const
{
    return mGlobalEntryMap;
}

RestoredEntries const&
GlobalParallelApplyLedgerState::getRestoredEntries() const
{
    return mGlobalRestoredEntries;
}

void
GlobalParallelApplyLedgerState::commitChangeFromThread(
    LedgerKey const& key, ParallelApplyEntry const& parEntry,
    std::unordered_set<LedgerKey> const& readWriteSet)
{
    if (!parEntry.mIsDirty)
    {
        return;
    }
    auto [it, inserted] = mGlobalEntryMap.emplace(key, parEntry);
    if (!inserted)
    {
        if (!maybeMergeRoTTLBumps(key, parEntry, it->second, readWriteSet))
        {
            it->second = parEntry;
        }
    }
}

void
GlobalParallelApplyLedgerState::commitChangesFromThread(
    AppConnector& app, ThreadParallelApplyLedgerState const& thread,
    ApplyStage const& stage)
{
    auto readWriteSet = getReadWriteKeysForStage(stage);
    for (auto const& [key, entry] : thread.getEntryMap())
    {
        commitChangeFromThread(key, entry, readWriteSet);
    }
    mGlobalRestoredEntries.addRestoresFrom(thread.getRestoredEntries());
}

void
GlobalParallelApplyLedgerState::commitChangesFromThreads(
    AppConnector& app,
    std::vector<std::unique_ptr<ThreadParallelApplyLedgerState>> const& threads,
    ApplyStage const& stage)
{
    releaseAssert(threadIsMain() ||
                  app.threadIsType(Application::ThreadType::APPLY));

    for (auto const& thread : threads)
    {
        commitChangesFromThread(app, *thread, stage);
    }
}

void
ThreadParallelApplyLedgerState::collectClusterFootprintEntriesFromGlobal(
    AppConnector& app, GlobalParallelApplyLedgerState const& global,
    Cluster const& cluster)
{
    releaseAssert(threadIsMain() ||
                  app.threadIsType(Application::ThreadType::APPLY));

    // As part of the initialization of this thread state, we need to
    // collect all the keys that are in the global state map. For any keys
    // we need not in the global state, we will fetch them from the live
    // snapshot, in memory soroban state, or the hot archive later.
    auto const& globalEntryMap = global.getGlobalEntryMap();

    auto fetchFromGlobal = [&](LedgerKey const& key) {
        if (mThreadEntryMap.find(key) != mThreadEntryMap.end())
        {
            return;
        }

        auto entryIt = globalEntryMap.find(key);
        if (entryIt != globalEntryMap.end())
        {
            if (entryIt->second.mLedgerEntry)
            {
                mThreadEntryMap.emplace(
                    key, ParallelApplyEntry::cleanPopulated(
                             entryIt->second.mLedgerEntry.value()));
            }
            else
            {
                mThreadEntryMap.emplace(key, ParallelApplyEntry::cleanEmpty());
            }
        }
    };

    for (auto const& txBundle : cluster)
    {
        auto const& footprint = txBundle.getTx()->sorobanResources().footprint;
        for (auto const& keys : {footprint.readWrite, footprint.readOnly})
        {
            for (auto const& key : keys)
            {
                fetchFromGlobal(key);
                if (isSorobanEntry(key))
                {
                    auto ttlKey = getTTLKey(key);
                    fetchFromGlobal(ttlKey);
                }
            }
        }
    }
}

ThreadParallelApplyLedgerState::ThreadParallelApplyLedgerState(
    AppConnector& app, GlobalParallelApplyLedgerState const& global,
    Cluster const& cluster)
    // TODO: find a way to clone these from parent rather than asking the
    // snapshot manager again. That might have changed! NB taking a shared
    // pointer copy is not safe, the snapshot objects are not threadsafe.
    : mHotArchiveSnapshot(app.copySearchableHotArchiveBucketListSnapshot())
    , mLiveSnapshot(app.copySearchableLiveBucketListSnapshot())
    , mInMemorySorobanState(global.mInMemorySorobanState)
    , mSorobanConfig(global.mSorobanConfig)
{
    releaseAssertOrThrow(global.getSnapshotLedgerSeq() ==
                         getSnapshotLedgerSeq());
    mPreviouslyRestoredEntries.addRestoresFrom(global.getRestoredEntries());
    collectClusterFootprintEntriesFromGlobal(app, global, cluster);
}

void
ThreadParallelApplyLedgerState::flushRoTTLBumpsInTxWriteFootprint(
    const TxBundle& txBundle)
{
    auto const& readWrite =
        txBundle.getTx()->sorobanResources().footprint.readWrite;

    for (auto const& lk : readWrite)
    {
        if (!isSorobanEntry(lk))
        {
            continue;
        }

        auto const& ttlKey = getTTLKey(lk);
        auto b = mRoTTLBumps.find(ttlKey);
        if (b != mRoTTLBumps.end())
        {
            // If we have residual RO TTL bumps for this key,
            // the entry must exist. If it was deleted, we would've
            // erased the TTL key from mRoTTLBumps.
            auto ttlEntry = getLiveEntryOpt(ttlKey);
            releaseAssertOrThrow(ttlEntry);
            releaseAssertOrThrow(ttl(ttlEntry) <= b->second);
            ttl(ttlEntry) = b->second;
            upsertEntry(ttlKey, ttlEntry.value(), getSnapshotLedgerSeq() + 1);
            mRoTTLBumps.erase(b);
        }
    }
}

void
ThreadParallelApplyLedgerState::flushRemainingRoTTLBumps()
{
    for (auto const& [lk, ttlBump] : mRoTTLBumps)
    {
        auto entryOpt = getLiveEntryOpt(lk);
        // The entry should always exist. If the entry was deleted,
        // then we would've erased the TTL key from roTTLBumps.
        releaseAssertOrThrow(entryOpt);
        if (ttl(entryOpt) < ttlBump)
        {
            auto updated = entryOpt.value();
            ttl(updated) = ttlBump;
            upsertEntry(lk, updated, getSnapshotLedgerSeq() + 1);
        }
    }
}

ParallelApplyEntryMap const&
ThreadParallelApplyLedgerState::getEntryMap() const
{
    return mThreadEntryMap;
}

RestoredEntries const&
ThreadParallelApplyLedgerState::getRestoredEntries() const
{
    return mThreadRestoredEntries;
}

std::optional<LedgerEntry>
ThreadParallelApplyLedgerState::getLiveEntryOpt(LedgerKey const& key) const
{
    auto it0 = mThreadEntryMap.find(key);
    if (it0 != mThreadEntryMap.end())
    {
        return it0->second.mLedgerEntry;
    }
    // Invariant check: If an entry was restored from the live state, then it's
    // possible that the thread entry map does not have that key (because live
    // restores only update the ttl), but if the entry was restored from the hot
    // archive, both the ttl entry and the entry itself are updated. So if the
    // key is missing from the thread entry map, it could not have been
    // previously restored from the hot archive.

    releaseAssertOrThrow(!mThreadRestoredEntries.entryWasRestoredFromMap(
        key, mThreadRestoredEntries.hotArchive));

    // mThreadEntryMap was preloaded with entries from the global map in
    // collectClusterFootprintEntriesFromGlobal (even if it's marked for
    // deletion), so if the keys does not exist in mThreadEntryMap, it can't
    // exist in the global entry map either. We still need to check the in
    // memory soroban state or the live snapshot.

    // Check InMemorySorobanState cache for soroban types
    std::shared_ptr<LedgerEntry const> res;
    if (InMemorySorobanState::isInMemoryType(key))
    {
        res = mInMemorySorobanState.get(key);
    }
    else
    {
        res = mLiveSnapshot->load(key);
    }

    return res ? std::make_optional(*res) : std::nullopt;
}

void
ThreadParallelApplyLedgerState::upsertEntry(LedgerKey const& key,
                                            LedgerEntry const& entry,
                                            uint32_t ledgerSeq)
{
    // Weird syntax avoid extra map lookup
    auto& mapEntry = mThreadEntryMap[key] =
        ParallelApplyEntry::dirtyPopulated(entry);
    mapEntry.mLedgerEntry->lastModifiedLedgerSeq = ledgerSeq;
}
void
ThreadParallelApplyLedgerState::eraseEntry(LedgerKey const& key)
{
    mThreadEntryMap[key] = ParallelApplyEntry::dirtyEmpty();
}

void
ThreadParallelApplyLedgerState::commitChangeFromSuccessfulOp(
    LedgerKey const& key, std::optional<LedgerEntry> const& entryOpt,
    UnorderedSet<LedgerKey> const& roTTLSet)
{
    auto oldEntryOpt = getLiveEntryOpt(key);
    if (entryOpt && oldEntryOpt && roTTLSet.find(key) != roTTLSet.end())
    {
        // Accumulate RO bumps instead of writing them to the entryMap.
        releaseAssertOrThrow(ttl(entryOpt) >= ttl(oldEntryOpt));
        updateMaxOfRoTTLBump(mRoTTLBumps, key, entryOpt);
    }
    else if (entryOpt)
    {
        upsertEntry(key, entryOpt.value(), getSnapshotLedgerSeq() + 1);
    }
    else
    {
        eraseEntry(key);
    }
}

void
ThreadParallelApplyLedgerState::setEffectsDeltaFromSuccessfulOp(
    ParallelTxReturnVal const& res, ParallelLedgerInfo const& ledgerInfo,
    TxEffects& effects) const
{
    releaseAssertOrThrow(res.getSuccess());
    for (auto const& [lk, le] : res.getModifiedEntryMap())
    {
        auto prevLe = getLiveEntryOpt(lk);
        LedgerTxnDelta::EntryDelta entryDelta;
        if (prevLe)
        {
            entryDelta.previous =
                std::make_shared<InternalLedgerEntry>(*prevLe);
        }
        else
        {
            // If the entry was not found in the live snapshot, we check if it
            // was restored from the hot archive instead.
            auto const& hotArchiveRestores =
                res.getRestoredEntries().hotArchive;
            auto it = hotArchiveRestores.find(lk);
            if (it != hotArchiveRestores.end())
            {
                entryDelta.previous =
                    std::make_shared<InternalLedgerEntry>(it->second);
            }
        }

        if (le)
        {
            entryDelta.current = std::make_shared<InternalLedgerEntry>(*le);
        }
        releaseAssertOrThrow(entryDelta.current || entryDelta.previous);
        effects.setDeltaEntry(lk, entryDelta);
    }
}

void
ThreadParallelApplyLedgerState::commitChangesFromSuccessfulOp(
    ParallelTxReturnVal const& res, TxBundle const& txBundle)
{
    releaseAssertOrThrow(res.getSuccess());
    auto roTTLSet = buildRoTTLSet(txBundle);
    for (auto const& [key, entryOpt] : res.getModifiedEntryMap())
    {
        commitChangeFromSuccessfulOp(key, entryOpt, roTTLSet);
    }
    mThreadRestoredEntries.addRestoresFrom(res.getRestoredEntries());
}

bool
ThreadParallelApplyLedgerState::entryWasRestored(LedgerKey const& key) const
{
    return mThreadRestoredEntries.entryWasRestored(key) ||
           mPreviouslyRestoredEntries.entryWasRestored(key);
}

uint32_t
ThreadParallelApplyLedgerState::getSnapshotLedgerSeq() const
{
    releaseAssertOrThrow(mLiveSnapshot->getLedgerSeq() ==
                         mHotArchiveSnapshot->getLedgerSeq());
    releaseAssertOrThrow(mLiveSnapshot->getLedgerSeq() ==
                         mInMemorySorobanState.getLedgerSeq());
    return mLiveSnapshot->getLedgerSeq();
}

SorobanNetworkConfig const&
ThreadParallelApplyLedgerState::getSorobanConfig() const
{
    return mSorobanConfig;
}

SearchableHotArchiveSnapshotConstPtr const&
ThreadParallelApplyLedgerState::getHotArchiveSnapshot() const
{
    return mHotArchiveSnapshot;
}

OpParallelApplyLedgerState::OpParallelApplyLedgerState(
    ThreadParallelApplyLedgerState const& parent)
    : mThreadState(parent)
{
}

std::optional<LedgerEntry>
OpParallelApplyLedgerState::getLiveEntryOpt(LedgerKey const& key) const
{
    // Note: most of the time we expect to be calling this function on an empty
    // mOpEntryMap -- during op setup -- and so to find no entries in
    // mOpEntryMap and read through to the underlying mThreadState. But it's
    // less risky if we don't have to rely on that fact or ensure it in callers:
    // if callers will get a consistent view of data even if the code changes
    // and we wind up with some new path calling with a non-empty mOpEntryMap.
    auto entryIter = mOpEntryMap.find(key);
    if (entryIter != mOpEntryMap.end())
    {
        return entryIter->second;
    }
    else
    {
        return mThreadState.getLiveEntryOpt(key);
    }
}

bool
OpParallelApplyLedgerState::upsertEntry(LedgerKey const& key,
                                        LedgerEntry const& entry,
                                        uint32_t ledgerSeq)
{
    // There are 4 cases:
    //
    //  1. The entry exists in the parent maps (thread state or live snapshot)
    //     but not in mOpEntryMap: we insert it into mOpEntryMap. This is a
    //     "logical update" even though it's a local insert. We return false.
    //
    //  2. The entry exists in the parent maps _and_ mOpEntryMap: we update it.
    //     This is obviously an update! We return false.
    //
    //  3. The entry does not exist in the parent maps but does already exist in
    //     mOpEntryMap: we update it. This is a "logical update" to an _earlier_
    //     logical create. We return false.
    //
    //  4. The entry does not exist in the parent maps and does not exist in
    //     mOpEntryMap: we insert it into mOpEntryMap. This is a "logical
    //     create". We return true.
    //
    // The only caller that cares about the return value is a loop that checks
    // that logical creates that happened in the soroban host were accompanied
    // by logical creates of TTL entries. We could theoretically return true in
    // case 3 by comparing against the op prestate rather than the local op
    // state, but the only time that happens is when there was a restore that
    // populated mOpEntryMap before invoking the host, and we don't especially
    // need to check our own TTL-creating work in that case.

    bool liveEntryExistedAlready = getLiveEntryOpt(key).has_value();
    CLOG_TRACE(Tx, "parallel apply thread {} upserting {} key {}",
               std::this_thread::get_id(),
               liveEntryExistedAlready ? "already-live" : "new",
               xdr::xdr_to_string(key, "key"));

    // Weird syntax avoids redundant map lookup
    auto& mapEntry = mOpEntryMap[key] = entry;
    mapEntry->lastModifiedLedgerSeq = ledgerSeq;
    return !liveEntryExistedAlready;
}

bool
OpParallelApplyLedgerState::eraseEntryIfExists(LedgerKey const& key)
{
    bool liveEntryExistedAlready = getLiveEntryOpt(key).has_value();
    if (liveEntryExistedAlready)
    {
        // NB: we only erase an entry if it doesn't already exist in
        // parents (thread state or live snapshot), otherwise
        // we will produce mismatched erases that don't relate to
        // any pre-state key when calculating the ledger delta.
        CLOG_TRACE(Tx, "parallel apply thread {} erasing {}",
                   std::this_thread::get_id(), xdr::xdr_to_string(key, "key"));
        mOpEntryMap[key] = std::nullopt;
    }
    else
    {
        CLOG_TRACE(Tx,
                   "parallel apply thread {} ignoring erase of non-existing "
                   "key {}",
                   std::this_thread::get_id(), xdr::xdr_to_string(key, "key"));
    }
    return liveEntryExistedAlready;
}

bool
OpParallelApplyLedgerState::entryWasRestored(LedgerKey const& key) const
{
    if (mOpRestoredEntries.entryWasRestored(key))
    {
        return true;
    }
    return mThreadState.entryWasRestored(key);
}

void
OpParallelApplyLedgerState::addHotArchiveRestore(LedgerKey const& key,
                                                 LedgerEntry const& entry,
                                                 LedgerKey const& ttlKey,
                                                 LedgerEntry const& ttlEntry)
{
    CLOG_TRACE(Tx, "parallel apply thread {} hot-restoring {}",
               std::this_thread::get_id(), xdr::xdr_to_string(key, "key"));
    mOpRestoredEntries.addHotArchiveRestore(key, entry, ttlKey, ttlEntry);
}

void
OpParallelApplyLedgerState::addLiveBucketlistRestore(
    LedgerKey const& key, LedgerEntry const& entry, LedgerKey const& ttlKey,
    LedgerEntry const& ttlEntry)
{
    CLOG_TRACE(Tx, "parallel apply thread {} live-restoring {}",
               std::this_thread::get_id(), xdr::xdr_to_string(key, "key"));
    mOpRestoredEntries.addLiveBucketlistRestore(key, entry, ttlKey, ttlEntry);
}

ParallelTxReturnVal
OpParallelApplyLedgerState::takeSuccess()
{
    CLOG_TRACE(Tx, "parallel apply thread {} succeeded with {} dirty entries",
               std::this_thread::get_id(), mOpEntryMap.size());
    return ParallelTxReturnVal{true, std::move(mOpEntryMap),
                               std::move(mOpRestoredEntries)};
}

ParallelTxReturnVal
OpParallelApplyLedgerState::takeFailure()
{
    CLOG_TRACE(Tx, "parallel apply thread {} failed with {} dirty entries",
               std::this_thread::get_id(), mOpEntryMap.size());
    return ParallelTxReturnVal{false, {}};
}

uint32_t
OpParallelApplyLedgerState::getSnapshotLedgerSeq() const
{
    return mThreadState.getSnapshotLedgerSeq();
}
}
