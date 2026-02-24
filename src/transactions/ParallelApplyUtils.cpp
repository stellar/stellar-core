// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/ParallelApplyUtils.h"
#include "bucket/BucketSnapshotManager.h"
#include "bucket/BucketUtils.h"
#include "ledger/LedgerEntryScope.h"
#include "ledger/LedgerTxn.h"
#include "ledger/NetworkConfig.h"
#include "main/AppConnector.h"
#include "transactions/MutableTransactionResult.h"
#include "transactions/ParallelApplyStage.h"
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


// Accumulate into the buffer of `roTTLBumps` the max of any existing entry and
// the provided `updatedLE`, which must be a non-nullopt TTL LE.
void
updateMaxOfRoTTLBump(UnorderedMap<LedgerKey, uint32_t>& roTTLBumps,
                     LedgerKey const& lk, LedgerEntry const& updatedLe)
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

std::unordered_set<LedgerKey>
getReadWriteKeysForStage(ApplyStage const& stage)
{
    ZoneScoped;
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
    : mLedgerInfo(ledgerInfo), mTxState(threadState)
{
    releaseAssertOrThrow(ledgerInfo.getLedgerSeq() ==
                         threadState.getSnapshotLedgerSeq() + 1);
}

std::optional<LedgerEntry>
ParallelLedgerAccessHelper::getLedgerEntryOpt(LedgerKey const& key)
{
    TxParApplyLedgerEntryOpt scopedOpt = mTxState.getLiveEntryOpt(key);
    return scopedOpt.readInScope(mTxState);
}

uint32_t
ParallelLedgerAccessHelper::getLedgerSeq()
{
    auto applySeq = mLedgerInfo.getLedgerSeq();
    releaseAssertOrThrow(applySeq == mTxState.getSnapshotLedgerSeq() + 1);
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
    return mTxState.upsertEntry(key, entry, mLedgerInfo.getLedgerSeq());
}

void
ParallelLedgerAccessHelper::upsertLedgerEntryKnownExisting(
    LedgerKey const& key, LedgerEntry const& entry)
{
    mTxState.upsertEntryKnownExisting(key, entry, mLedgerInfo.getLedgerSeq());
}

bool
ParallelLedgerAccessHelper::eraseLedgerEntryIfExists(LedgerKey const& key)
{
    return mTxState.eraseEntryIfExists(key);
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
    : LedgerEntryScope(ScopeIdT(0, ltx.getHeader().ledgerSeq))
    , mHotArchiveSnapshot(app.copySearchableHotArchiveBucketListSnapshot())
    , mLiveSnapshot(app.copySearchableLiveBucketListSnapshot())
    , mInMemorySorobanState(inMemoryState)
    , mSorobanConfig(sorobanConfig)
{
    ZoneScoped;
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
    preParallelApplyAndCollectModifiedClassicEntries(app, ltx, stages);
}

void
GlobalParallelApplyLedgerState::
    preParallelApplyAndCollectModifiedClassicEntries(
        AppConnector& app, AbstractLedgerTxn& ltx,
        std::vector<ApplyStage> const& stages)
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

                GlobalParApplyLedgerEntryOpt entry = scopeAdoptEntryOpt(
                    entryPair.second
                        ? std::make_optional(entryPair.second->ledgerEntry())
                        : std::nullopt);

                mGlobalEntryMap.emplace(lk,
                                        GlobalParallelApplyEntry{entry, false});
            }
        };

    // First call preParallelApply on all transactions,
    // and then load from footprints. This order is important
    // because preParallelApply modifies the fee source accounts
    // and those accounts could show up in the footprint
    // of a different transaction.
    {
        ZoneNamedN(preApplyZone, "preParallelApply all txs", true);
        for (auto const& stage : stages)
        {
            for (auto const& txBundle : stage)
            {
                // Make sure to call preParallelApply on all txs because this
                // will modify the fee source accounts sequence numbers.
                txBundle.getTx()->preParallelApply(
                    app, ltx, txBundle.getEffects().getMeta(),
                    txBundle.getResPayload(), mSorobanConfig);
            }
        }
    }

    {
        ZoneNamedN(fetchZone, "fetchClassicEntries from footprints", true);
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

    // Pre-load Soroban read-only entries (and their TTLs) from
    // InMemorySorobanState into the global entry map. Without this,
    // every thread-level getLiveEntryOpt for a read-only Soroban key
    // falls through to InMemorySorobanState::get() (involving hash
    // computation and LedgerEntry copy). For workloads like SAC
    // transfers where all TXs share the same read-only entries
    // (contract instance), this saves thousands of redundant lookups
    // per thread.
    //
    // Note: Only RO entries benefit from pre-loading because they are
    // shared across many TXs. RW entries are unique per TX (e.g.
    // balance entries), so pre-loading them just moves the
    // InMemorySorobanState load from per-TX time to setup time and
    // ADDS overhead from global->thread->TX copy chain.
    {
        ZoneNamedN(fetchSorobanRoZone,
                   "fetchSorobanReadOnlyEntries from footprints", true);
        for (auto const& stage : stages)
        {
            for (auto const& txBundle : stage)
            {
                for (auto const& lk :
                     txBundle.getTx()->sorobanResources().footprint.readOnly)
                {
                    if (!isSorobanEntry(lk))
                    {
                        continue;
                    }
                    if (mGlobalEntryMap.find(lk) != mGlobalEntryMap.end())
                    {
                        continue;
                    }

                    std::shared_ptr<LedgerEntry const> res;
                    if (InMemorySorobanState::isInMemoryType(lk))
                    {
                        res = mInMemorySorobanState.get(lk);
                    }
                    else
                    {
                        res = mLiveSnapshot->load(lk);
                    }

                    if (res)
                    {
                        GlobalParApplyLedgerEntryOpt entry =
                            scopeAdoptEntryOpt(
                                std::make_optional(*res));
                        mGlobalEntryMap.emplace(
                            lk,
                            GlobalParallelApplyEntry{entry, false});

                        // Also pre-load the TTL entry
                        auto ttlKey = getTTLKey(lk);
                        if (mGlobalEntryMap.find(ttlKey) ==
                            mGlobalEntryMap.end())
                        {
                            std::shared_ptr<LedgerEntry const> ttlRes;
                            if (InMemorySorobanState::isInMemoryType(ttlKey))
                            {
                                ttlRes =
                                    mInMemorySorobanState.get(ttlKey);
                            }
                            else
                            {
                                ttlRes = mLiveSnapshot->load(ttlKey);
                            }
                            if (ttlRes)
                            {
                                GlobalParApplyLedgerEntryOpt ttlEntry =
                                    scopeAdoptEntryOpt(
                                        std::make_optional(*ttlRes));
                                mGlobalEntryMap.emplace(
                                    ttlKey,
                                    GlobalParallelApplyEntry{ttlEntry,
                                                             false});
                            }
                        }
                    }
                }
            }
        }
    }
}

void
GlobalParallelApplyLedgerState::commitChangesToLedgerTxn(
    AbstractLedgerTxn& ltx)
{
    ZoneScoped;
    for (auto& [key, entry] : mGlobalEntryMap)
    {
        // Only update if dirty bit is set
        if (!entry.mIsDirty)
        {
            continue;
        }

        // Move the LedgerEntry out of the scoped wrapper. This is safe
        // because commitChangesToLedgerTxn is the final operation on the
        // global state — it is destroyed immediately after this call.
        auto movedLe = entry.mLedgerEntry.moveFromScope(*this);
        if (movedLe)
        {
            // Use the mIsNew flag tracked during the parallel apply phase to
            // decide between createWithoutLoading (INIT) and
            // updateWithoutLoading (LIVE). This avoids the expensive per-entry
            // existence check (mInMemorySorobanState.get() does SHA256 per
            // CONTRACT_DATA key, and getNewestVersionBelowRoot does a hash map
            // lookup for classic entries).
            InternalLedgerEntry ile(std::move(*movedLe));
            if (entry.mIsNew)
            {
                ltx.createWithoutLoading(std::move(ile));
            }
            else
            {
                ltx.updateWithoutLoading(std::move(ile));
            }
        }
        else
        {
            // Delete case: use load() + erase() to maintain EXACT consistency.
            // Deletes are rare in SAC transfers, so the cost is negligible.
            auto ltxe = ltx.load(key);
            if (ltxe)
            {
                ltx.erase(key);
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
            ltx.markRestoredFromHotArchive(kvp.second, it->second);
        }
    }
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

GlobalParallelApplyEntryMap const&
GlobalParallelApplyLedgerState::getGlobalEntryMap() const
{
    return mGlobalEntryMap;
}

RestoredEntries const&
GlobalParallelApplyLedgerState::getRestoredEntries() const
{
    return mGlobalRestoredEntries;
}

bool
GlobalParallelApplyLedgerState::maybeMergeRoTTLBumps(
    LedgerKey const& key, GlobalParallelApplyEntry const& newEntry,
    GlobalParallelApplyEntry& oldEntry,
    std::unordered_set<LedgerKey> const& readWriteSet)
{
    // Read Only bumps will always be updating a pre-existing value. TTL
    // creation (!oldEntry) or deletion (!newEntry) are write conflicts that
    // don't have merge special casing.
    std::optional<LedgerEntry> const& newLe =
        newEntry.mLedgerEntry.readInScope(*this);
    auto merged = false;
    oldEntry.mLedgerEntry.modifyInScope(
        *this, [&](std::optional<LedgerEntry>& oldLe) {
            if (newLe && oldLe && key.type() == TTL)
            {
                releaseAssertOrThrow(newLe.value().data.type() == TTL);
                releaseAssertOrThrow(oldLe.value().data.type() == TTL);
                if (readWriteSet.find(key) == readWriteSet.end())
                {
                    uint32_t const& newTTL = ttl(newLe);
                    uint32_t& oldTTL = ttl(oldLe);
                    oldTTL = std::max(oldTTL, newTTL);
                    // Propagate lastModifiedLedgerSeq from the thread's
                    // entry. This is necessary when the old entry was
                    // pre-loaded with a stale lastModifiedLedgerSeq.
                    oldLe.value().lastModifiedLedgerSeq =
                        newLe.value().lastModifiedLedgerSeq;
                    merged = true;
                }
            }
        });
    return merged;
}

void
GlobalParallelApplyLedgerState::commitChangeFromThread(
    ThreadParallelApplyLedgerState const& thread, LedgerKey const& key,
    ThreadParallelApplyEntry const& parEntry,
    std::unordered_set<LedgerKey> const& readWriteSet)
{
    if (!parEntry.mIsDirty)
    {
        return;
    }
    auto rescopedParEntry = parEntry.rescope(thread, *this);
    auto [it, inserted] = mGlobalEntryMap.emplace(key, rescopedParEntry);
    if (!inserted)
    {
        if (!maybeMergeRoTTLBumps(key, rescopedParEntry, it->second,
                                  readWriteSet))
        {
            // Preserve mIsNew from the first stage that touched this entry.
            bool oldIsNew = it->second.mIsNew;
            it->second = rescopedParEntry;
            it->second.mIsNew = oldIsNew;
        }
        else
        {
            // The merge modified the entry value in-place. Mark it dirty
            // so commitChangesToLedgerTxn writes it. This is necessary
            // when the entry was pre-loaded (with mIsDirty=false) by the
            // Soroban RO entry pre-loading in the constructor.
            it->second.mIsDirty = true;
        }
    }
}

void
GlobalParallelApplyLedgerState::commitChangesFromThread(
    AppConnector& app, ThreadParallelApplyLedgerState const& thread,
    std::unordered_set<LedgerKey> const& readWriteSet)
{
    ZoneScoped;
    thread.scopeDeactivate();
    for (auto const& [key, entry] : thread.getEntryMap())
    {
        commitChangeFromThread(thread, key, entry, readWriteSet);
    }
    mGlobalRestoredEntries.addRestoresFrom(thread.getRestoredEntries());
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
    GlobalParallelApplyEntryMap const& globalEntryMap =
        global.getGlobalEntryMap();

    auto fetchFromGlobal = [&](LedgerKey const& key) {
        if (mThreadEntryMap.find(key) != mThreadEntryMap.end())
        {
            return;
        }

        auto entryIt = globalEntryMap.find(key);
        if (entryIt != globalEntryMap.end())
        {
            auto threadEntry = ThreadParallelApplyEntry::clean(
                scopeAdoptEntryOptFrom(entryIt->second.mLedgerEntry, global));
            // Propagate mIsNew from global so subsequent upserts preserve it.
            threadEntry.mIsNew = entryIt->second.mIsNew;
            mThreadEntryMap.emplace(key, threadEntry);
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
                    // Use TTL key cache to avoid redundant SHA-256
                    // computation for repeated keys across TXs in
                    // the cluster.
                    auto [cacheIt, inserted] =
                        mTTLKeyCache.try_emplace(key, LedgerKey{});
                    if (inserted)
                    {
                        cacheIt->second = getTTLKey(key);
                    }
                    fetchFromGlobal(cacheIt->second);
                }
            }
        }
    }
}

ThreadParallelApplyLedgerState::ThreadParallelApplyLedgerState(
    AppConnector& app, GlobalParallelApplyLedgerState const& global,
    Cluster const& cluster, size_t clusterIdx)
    : LedgerEntryScope(ScopeIdT(clusterIdx, global.mScopeID.mLedger))
    // TODO: find a way to clone these from parent rather than asking the
    // snapshot manager again. That might have changed! NB taking a shared
    // pointer copy is not safe, the snapshot objects are not threadsafe.
    , mHotArchiveSnapshot(app.copySearchableHotArchiveBucketListSnapshot())
    , mLiveSnapshot(app.copySearchableLiveBucketListSnapshot())
    , mInMemorySorobanState(global.mInMemorySorobanState)
    , mSorobanConfig(global.mSorobanConfig)
    , mModuleCache(app.getModuleCache())
{
    releaseAssertOrThrow(global.getSnapshotLedgerSeq() ==
                         getSnapshotLedgerSeq());
    mPreviouslyRestoredEntries.addRestoresFrom(global.getRestoredEntries());
    collectClusterFootprintEntriesFromGlobal(app, global, cluster);
}

void
ThreadParallelApplyLedgerState::flushRoTTLBumpsInTxWriteFootprint(
    TxBundle const& txBundle)
{
    auto const& readWrite =
        txBundle.getTx()->sorobanResources().footprint.readWrite;

    for (auto const& lk : readWrite)
    {
        if (!isSorobanEntry(lk))
        {
            continue;
        }

        // Use TTL key cache to avoid redundant SHA-256 computation.
        auto cacheIt = mTTLKeyCache.find(lk);
        releaseAssertOrThrow(cacheIt != mTTLKeyCache.end());
        auto const& ttlKey = cacheIt->second;
        auto b = mRoTTLBumps.find(ttlKey);
        if (b != mRoTTLBumps.end())
        {
            // If we have residual RO TTL bumps for this key,
            // the entry must exist. If it was deleted, we would've
            // erased the TTL key from mRoTTLBumps.
            ThreadParApplyLedgerEntryOpt scopedTtlEntryOpt =
                getLiveEntryOpt(ttlKey);
            scopedTtlEntryOpt.modifyInScope(
                *this, [&](std::optional<LedgerEntry>& ttlEntryOpt) {
                    releaseAssertOrThrow(ttlEntryOpt);
                    LedgerEntry& ttlEntry = ttlEntryOpt.value();
                    releaseAssertOrThrow(ttl(ttlEntry) <= b->second);
                    ttl(ttlEntry) = b->second;
                    upsertEntry(ttlKey, scopeAdoptEntry(ttlEntry),
                                getSnapshotLedgerSeq() + 1);
                });
            mRoTTLBumps.erase(b);
        }
    }
}

void
ThreadParallelApplyLedgerState::flushRemainingRoTTLBumps()
{
    for (auto const& kvp : mRoTTLBumps)
    {
        auto const& lk = kvp.first;
        auto const& ttlBump = kvp.second;
        ThreadParApplyLedgerEntryOpt scopedEntryOpt = getLiveEntryOpt(lk);
        // The entry should always exist. If the entry was deleted,
        // then we would've erased the TTL key from roTTLBumps.
        scopedEntryOpt.modifyInScope(
            *this, [&](std::optional<LedgerEntry>& entryOpt) {
                releaseAssertOrThrow(entryOpt);
                releaseAssertOrThrow(entryOpt);
                LedgerEntry& entry = entryOpt.value();
                if (ttl(entry) < ttlBump)
                {
                    ttl(entry) = ttlBump;
                    upsertEntry(lk, scopeAdoptEntry(entry),
                                getSnapshotLedgerSeq() + 1);
                }
            });
    }
}

ThreadParallelApplyEntryMap const&
ThreadParallelApplyLedgerState::getEntryMap() const
{
    return mThreadEntryMap;
}

RestoredEntries const&
ThreadParallelApplyLedgerState::getRestoredEntries() const
{
    return mThreadRestoredEntries;
}

ThreadParallelApplyLedgerState::OptionalEntryT
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

    return scopeAdoptEntryOpt(res ? std::make_optional(*res) : std::nullopt);
}

void
ThreadParallelApplyLedgerState::upsertEntry(
    LedgerKey const& key, ThreadParApplyLedgerEntry const& entry,
    uint32_t ledgerSeq, bool isNew)
{
    auto parAppEntry = ThreadParallelApplyEntry::dirty(entry);
    parAppEntry.mLedgerEntry.modifyInScope(
        *this, [&](std::optional<LedgerEntry>& le) {
            releaseAssertOrThrow(le);
            le.value().lastModifiedLedgerSeq = ledgerSeq;
        });
    // Use try_emplace to preserve mIsNew from the first touch of this entry.
    // If the entry already exists in the thread map (from collectCluster or a
    // previous TX), keep its mIsNew flag. Otherwise use the caller's isNew.
    parAppEntry.mIsNew = isNew;
    auto [it, inserted] = mThreadEntryMap.try_emplace(key, parAppEntry);
    if (!inserted)
    {
        parAppEntry.mIsNew = it->second.mIsNew;
        it->second = parAppEntry;
    }
}
void
ThreadParallelApplyLedgerState::eraseEntry(LedgerKey const& key, bool isNew)
{
    auto parAppEntry =
        ThreadParallelApplyEntry::dirty(scopeAdoptEntryOpt(std::nullopt));
    // Preserve mIsNew from previous touch, or use caller's isNew for first
    // touch. This matters when a subsequent TX recreates the entry: the
    // preserved flag determines INIT vs LIVE in commitChangesToLedgerTxn.
    parAppEntry.mIsNew = isNew;
    auto [it, inserted] = mThreadEntryMap.try_emplace(key, parAppEntry);
    if (!inserted)
    {
        parAppEntry.mIsNew = it->second.mIsNew;
        it->second = parAppEntry;
    }
}

void
ThreadParallelApplyLedgerState::commitChangeFromSuccessfulTx(
    LedgerKey const& key, ThreadParApplyLedgerEntryOpt const& newScopedEntryOpt,
    xdr::xvector<LedgerKey> const& roFootprint)
{
    ThreadParApplyLedgerEntryOpt oldScopedEntryOpt = getLiveEntryOpt(key);
    std::optional<LedgerEntry> const& oldEntryOpt =
        oldScopedEntryOpt.readInScope(*this);
    std::optional<LedgerEntry> const& newEntryOpt =
        newScopedEntryOpt.readInScope(*this);

    // Check if this key is a TTL key for a read-only Soroban entry by
    // scanning the TX's RO footprint with cached TTL key lookups.
    // This avoids building a per-TX UnorderedSet (hash + emplace per RO
    // entry) and instead does a small linear scan (typically 2-4 entries
    // for SAC transfers).
    bool isRoTTL = false;
    if (newEntryOpt && oldEntryOpt && key.type() == TTL)
    {
        for (auto const& ro : roFootprint)
        {
            if (!isSorobanEntry(ro))
            {
                continue;
            }
            auto cacheIt = mTTLKeyCache.find(ro);
            releaseAssertOrThrow(cacheIt != mTTLKeyCache.end());
            if (cacheIt->second == key)
            {
                isRoTTL = true;
                break;
            }
        }
    }

    if (isRoTTL)
    {
        auto const& entry = newEntryOpt.value();
        // Accumulate RO bumps instead of writing them to the entryMap.
        releaseAssertOrThrow(ttl(entry) >= ttl(oldEntryOpt.value()));
        updateMaxOfRoTTLBump(mRoTTLBumps, key, entry);
    }
    else if (newEntryOpt)
    {
        // If oldEntryOpt is null, the entry doesn't exist in any parent map
        // or persistent state — it's a newly created entry.
        bool isNew = !oldEntryOpt.has_value();
        upsertEntry(key, scopeAdoptEntry(newEntryOpt.value()),
                    getSnapshotLedgerSeq() + 1, isNew);
    }
    else
    {
        bool isNew = !oldEntryOpt.has_value();
        eraseEntry(key, isNew);
    }
}

void
ThreadParallelApplyLedgerState::setEffectsDeltaFromSuccessfulTx(
    ParallelTxReturnVal const& res, ParallelLedgerInfo const& ledgerInfo,
    TxEffects& effects) const
{
    ZoneScoped;
    releaseAssertOrThrow(res.getSuccess());
    for (auto const& [lk, scopedEntryOpt] : res.getModifiedEntryMap())
    {
        ThreadParApplyLedgerEntryOpt prevScopedLe = getLiveEntryOpt(lk);
        std::optional<LedgerEntry> const& prevLe =
            prevScopedLe.readInScope(*this);
        LedgerTxnDelta::EntryDelta entryDelta;
        if (prevLe)
        {
            entryDelta.previous =
                std::make_shared<InternalLedgerEntry>(prevLe.value());
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

        auto entryOpt = scopedEntryOpt.readInScope(res);
        if (entryOpt)
        {
            entryDelta.current =
                std::make_shared<InternalLedgerEntry>(entryOpt.value());
        }
        releaseAssertOrThrow(entryDelta.current || entryDelta.previous);
        effects.setDeltaEntry(lk, entryDelta);
    }
}

void
ThreadParallelApplyLedgerState::commitChangesFromSuccessfulTx(
    ParallelTxReturnVal const& res, TxBundle const& txBundle)
{
    releaseAssertOrThrow(res.getSuccess());
    auto const& roFootprint =
        txBundle.getTx()->sorobanResources().footprint.readOnly;
    for (auto const& [key, txScopedEntryOpt] : res.getModifiedEntryMap())
    {
        auto threadScopedEntryOpt =
            scopeAdoptEntryOptFrom(txScopedEntryOpt, res);
        commitChangeFromSuccessfulTx(key, threadScopedEntryOpt, roFootprint);
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

rust::Box<rust_bridge::SorobanModuleCache> const&
ThreadParallelApplyLedgerState::getModuleCache() const
{
    return mModuleCache;
}

TxParallelApplyLedgerState::TxParallelApplyLedgerState(
    ThreadParallelApplyLedgerState const& parent)
    : LedgerEntryScope(
          ScopeIdT(parent.mScopeID.mIndex, parent.mScopeID.mLedger))
    , mThreadState(parent)
    , mThreadStateDeactivateGuard(mThreadState)
{
}

TxParallelApplyLedgerState::OptionalEntryT
TxParallelApplyLedgerState::getLiveEntryOpt(LedgerKey const& key) const
{
    // Note: most of the time we expect to be calling this function on an empty
    // mTxEntryMap -- during op setup -- and so to find no entries in
    // mTxEntryMap and read through to the underlying mThreadState. But it's
    // less risky if we don't have to rely on that fact or ensure it in callers:
    // if callers will get a consistent view of data even if the code changes
    // and we wind up with some new path calling with a non-empty mTxEntryMap.
    auto entryIter = mTxEntryMap.find(key);
    if (entryIter != mTxEntryMap.end())
    {
        return entryIter->second;
    }
    else
    {
        return scopeAdoptEntryOptFrom(mThreadState.getLiveEntryOpt(key),
                                      mThreadState);
    }
}

bool
TxParallelApplyLedgerState::upsertEntry(LedgerKey const& key,
                                        LedgerEntry const& entry,
                                        uint32_t ledgerSeq)
{
    ZoneScoped;
    // There are 4 cases:
    //
    //  1. The entry exists in the parent maps (thread state or live snapshot)
    //     but not in mTxEntryMap: we insert it into mTxEntryMap. This is a
    //     "logical update" even though it's a local insert. We return false.
    //
    //  2. The entry exists in the parent maps _and_ mTxEntryMap: we update it.
    //     This is obviously an update! We return false.
    //
    //  3. The entry does not exist in the parent maps but does already exist in
    //     mTxEntryMap: we update it. This is a "logical update" to an _earlier_
    //     logical create. We return false.
    //
    //  4. The entry does not exist in the parent maps and does not exist in
    //     mTxEntryMap: we insert it into mTxEntryMap. This is a "logical
    //     create". We return true.
    //
    // The only caller that cares about the return value is a loop that checks
    // that logical creates that happened in the soroban host were accompanied
    // by logical creates of TTL entries. We could theoretically return true in
    // case 3 by comparing against the op prestate rather than the local op
    // state, but the only time that happens is when there was a restore that
    // populated mTxEntryMap before invoking the host, and we don't especially
    // need to check our own TTL-creating work in that case.

    bool liveEntryExistedAlready =
        getLiveEntryOpt(key).readInScope(*this).has_value();
    CLOG_TRACE(Tx, "parallel apply thread {} upserting {} key {}",
               std::this_thread::get_id(),
               liveEntryExistedAlready ? "already-live" : "new",
               xdr::xdr_to_string(key, "key"));

    auto [mapEntry, _] =
        mTxEntryMap.insert_or_assign(key, scopeAdoptEntryOpt(entry));
    mapEntry->second.modifyInScope(*this, [&](std::optional<LedgerEntry>& le) {
        releaseAssertOrThrow(le);
        le.value().lastModifiedLedgerSeq = ledgerSeq;
    });
    return !liveEntryExistedAlready;
}

void
TxParallelApplyLedgerState::upsertEntryKnownExisting(
    LedgerKey const& key, LedgerEntry const& entry, uint32_t ledgerSeq)
{
    ZoneScoped;
    // Skip getLiveEntryOpt — caller guarantees entry already exists in parent
    // state, so this is always a logical update (not a create).
    auto [mapEntry, _] =
        mTxEntryMap.insert_or_assign(key, scopeAdoptEntryOpt(entry));
    mapEntry->second.modifyInScope(*this, [&](std::optional<LedgerEntry>& le) {
        releaseAssertOrThrow(le);
        le.value().lastModifiedLedgerSeq = ledgerSeq;
    });
}

bool
TxParallelApplyLedgerState::eraseEntryIfExists(LedgerKey const& key)
{
    bool liveEntryExistedAlready =
        getLiveEntryOpt(key).readInScope(*this).has_value();
    if (liveEntryExistedAlready)
    {
        // NB: we only erase an entry if it doesn't already exist in
        // parents (thread state or live snapshot), otherwise
        // we will produce mismatched erases that don't relate to
        // any pre-state key when calculating the ledger delta.
        CLOG_TRACE(Tx, "parallel apply thread {} erasing {}",
                   std::this_thread::get_id(), xdr::xdr_to_string(key, "key"));
        mTxEntryMap.insert_or_assign(key, scopeAdoptEntryOpt(std::nullopt));
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
TxParallelApplyLedgerState::entryWasRestored(LedgerKey const& key) const
{
    if (mTxRestoredEntries.entryWasRestored(key))
    {
        return true;
    }
    return mThreadState.entryWasRestored(key);
}

void
TxParallelApplyLedgerState::addHotArchiveRestore(LedgerKey const& key,
                                                 LedgerEntry const& entry,
                                                 LedgerKey const& ttlKey,
                                                 LedgerEntry const& ttlEntry)
{
    CLOG_TRACE(Tx, "parallel apply thread {} hot-restoring {}",
               std::this_thread::get_id(), xdr::xdr_to_string(key, "key"));
    mTxRestoredEntries.addHotArchiveRestore(key, entry, ttlKey, ttlEntry);
}

void
TxParallelApplyLedgerState::addLiveBucketlistRestore(
    LedgerKey const& key, LedgerEntry const& entry, LedgerKey const& ttlKey,
    LedgerEntry const& ttlEntry)
{
    CLOG_TRACE(Tx, "parallel apply thread {} live-restoring {}",
               std::this_thread::get_id(), xdr::xdr_to_string(key, "key"));
    mTxRestoredEntries.addLiveBucketlistRestore(key, entry, ttlKey, ttlEntry);
}

ParallelTxReturnVal
TxParallelApplyLedgerState::takeSuccess()
{
    CLOG_TRACE(Tx, "parallel apply thread {} succeeded with {} dirty entries",
               std::this_thread::get_id(), mTxEntryMap.size());

    return ParallelTxReturnVal{true, std::move(mTxEntryMap),
                               std::move(mTxRestoredEntries), mScopeID};
}

ParallelTxReturnVal
TxParallelApplyLedgerState::takeFailure()
{
    CLOG_TRACE(Tx, "parallel apply thread {} failed with {} dirty entries",
               std::this_thread::get_id(), mTxEntryMap.size());
    return ParallelTxReturnVal{false, {}, mScopeID};
}

uint32_t
TxParallelApplyLedgerState::getSnapshotLedgerSeq() const
{
    return mThreadState.getSnapshotLedgerSeq();
}
}
