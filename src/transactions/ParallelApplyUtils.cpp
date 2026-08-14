// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/ParallelApplyUtils.h"
#include "bucket/BucketUtils.h"
#include "ledger/LedgerEntryScope.h"
#include "ledger/LedgerTxn.h"
#include "ledger/NetworkConfig.h"
#include "main/AppConnector.h"
#include "transactions/OperationFrame.h"
#include "transactions/ParallelApplyStage.h"
#include "transactions/TransactionFrameBase.h"
#include "transactions/TransactionUtils.h"
#include "util/BatchExecutor.h"
#include "util/GlobalChecks.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdrpp/printer.h"
#include <fmt/core.h>
#include <fmt/std.h>
#include <thread>
#include <unordered_map>

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

// Construct a set of all the TTL keys associated with all RO soroban
// (code-or-data) keys named in the footprint of the `txBundle`. Note
// that since RO and RW footprints are disjoint, we only have to look
// at the RO set.
ParallelApplyLedgerKeySet
buildRoTTLSet(TxBundle const& txBundle)
{
    ParallelApplyLedgerKeySet isReadOnlyTTLSet;
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
updateMaxOfRoTTLBump(ParallelApplyLedgerKeyMap<uint32_t>& roTTLBumps,
                     ParallelApplyLedgerKey const& lk,
                     LedgerEntry const& updatedLe)
{
    auto [it, emplaced] = roTTLBumps.emplace(lk, ttl(updatedLe));
    if (!emplaced)
    {
        it->second = std::max(it->second, ttl(updatedLe));
    }
}

void
commitPreParallelApplyWrites(AppConnector& app, AbstractLedgerTxn& ltx,
                             std::vector<TxBundle const*> const& txBundles)
{
    ZoneScoped;
    for (auto const* txBundle : txBundles)
    {
        txBundle->getTx()->preParallelApplyWrite(
            app, ltx, txBundle->getEffects().getMeta(),
            txBundle->getResPayload());
    }
}

} // namespace

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
    AppConnector& app, ApplyLedgerView applyView, AbstractLedgerTxn& ltx,
    std::vector<ApplyStage> const& stages,
    InMemorySorobanState const& inMemoryState,
    SorobanNetworkConfig const& sorobanConfig)
    : LedgerEntryScope(ScopeIdT(0, ltx.getHeader().ledgerSeq))
    , mLCLApplyView(std::move(applyView))
    , mInMemorySorobanState(inMemoryState)
    , mSorobanConfig(sorobanConfig)
    , mGlobalEntryMap(
          std::max<size_t>(1, app.getBatchExecutor().preferredTaskCount()))
{
    releaseAssertOrThrow(mLCLApplyView.getLedgerSeq() ==
                         mInMemorySorobanState.getLedgerSeq());
    releaseAssertOrThrow(ltx.getHeader().ledgerSeq ==
                         mLCLApplyView.getLedgerSeq() + 1);
    releaseAssert(threadIsMain() ||
                  app.threadIsType(Application::ThreadType::APPLY));
    // From now on, we will be using globalState, liveSnapshots, and the
    // hotArchive to collect all entries. Before we continue though, we need to
    // load into the globalEntryMap any classic entries that have been modified
    // in this ledger because those changes won't be reflected in the
    // globalEntryMap. The entries that could've changed are accounts and
    // trustlines from the classic phase, as well as fee source accounts that
    // had their sequence numbers bumped and fees charged. preParallelApply will
    // update sequence numbers so it needs to be called before we check
    // LedgerTxn.
    preApplyAndCollectModifiedClassicEntries(app, ltx, stages);
}

void
GlobalParallelApplyLedgerState::preApplyAndCollectModifiedClassicEntries(
    AppConnector& app, AbstractLedgerTxn& ltx,
    std::vector<ApplyStage> const& stages)
{
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

                ParallelApplyLedgerKey pk(lk);
                globalMapShardFor(pk).emplace(
                    std::move(pk),
                    GlobalParallelApplyEntry::clean(
                        std::move(entry), /*isNew=*/!entryPair.second));
            }
        };

    std::vector<TxBundle const*> txBundles;
    for (auto const& stage : stages)
    {
        for (auto const& txBundle : stage)
        {
            txBundles.emplace_back(&txBundle);
        }
    }

    // Pre-apply all the transactions before loading the footprint entries. This
    // order is important because the pre-apply modifies the source accounts,
    // and those accounts could show up in the footprint of a transaction
    // applied by a different thread, thus breaking the invariant that
    // transactions are independent of each other across threads.
    //
    // The pre-apply process is done in two phases: a parallel read-only phase
    // where the transactions are validated, and a serial write phase where the
    // writes are committed to the ledger.
    //
    // This phase separatation hinges on the fact that the validation outcome
    // of any Soroban transaction can't be influenced by the pre-apply writes
    // performed by another Soroban transaction. Specifically, pre-apply writes
    // only include:
    // - The source account sequence number bumps - this is fine because we
    //   have only a single transaction per source account per ledger
    // - The removal of one-time pre-authorized tx signers - this is also fine
    //   because any given transaction in a ledger is unique, and increasing the
    //   sub-entry count of a source/sponsor account is not relevant at that
    //   point, as the fees have already been successfully charged.

    auto header =
        std::make_shared<LedgerHeader const>(ltx.loadHeader().current());
    readOnlyParallelPreApply(app, txBundles, header, ltx);
    commitPreParallelApplyWrites(app, ltx, txBundles);

    for (auto const& txBundle : txBundles)
    {
        auto const& footprint = txBundle->getTx()->sorobanResources().footprint;
        fetchInMemoryClassicEntries(footprint.readWrite);
        fetchInMemoryClassicEntries(footprint.readOnly);
    }
}

void
GlobalParallelApplyLedgerState::readOnlyParallelPreApply(
    AppConnector& app, std::vector<TxBundle const*> const& txBundles,
    std::shared_ptr<LedgerHeader const> header, AbstractLedgerTxn& ltx)
{
    ZoneScoped;
    if (txBundles.empty())
    {
        return;
    }

    // Run pre-apply for [begin, end) transaction indices.
    auto runRange = [&](size_t begin, size_t end) {
        // NB: mLCLApplyView is not thread-safe, so we need to copy it into a
        // thread-local view.
        CheckValidLedgerViewWrapper ledgerView(
            std::make_unique<SorobanPreApplyLedgerView>(header, ltx,
                                                        mLCLApplyView));
        for (size_t i = begin; i < end; ++i)
        {
            auto const* txBundle = txBundles[i];
            txBundle->getTx()->preParallelApplyReadOnly(
                app, ledgerView, txBundle->getEffects().getMeta(),
                txBundle->getResPayload(), mSorobanConfig);
        }
    };

    size_t taskCount = app.getBatchExecutor().preferredTaskCount();
    if (taskCount <= 1)
    {
        runRange(0, txBundles.size());
        return;
    }

    std::vector<std::function<int()>> tasks;
    tasks.reserve(taskCount);
    size_t begin = 0;
    size_t baseChunk = txBundles.size() / taskCount;
    size_t remainder = txBundles.size() % taskCount;
    for (size_t i = 0; i < taskCount; ++i)
    {
        size_t end = begin + baseChunk + (i < remainder ? 1 : 0);
        tasks.emplace_back([runRange, begin, end]() {
            runRange(begin, end);
            return 0;
        });
        begin = end;
    }
    releaseAssert(begin == txBundles.size());
    app.getBatchExecutor().executeBatch(std::move(tasks));
}

void
GlobalParallelApplyLedgerState::commitChangesToLedgerTxn(AbstractLedgerTxn& ltx)
{
    ZoneScoped;
    for (auto& shard : mGlobalEntryMap)
    {
        for (auto& [key, entry] : shard)
        {
            // Only update if dirty bit is set
            if (!entry.mIsDirty)
            {
                continue;
            }

            std::optional<LedgerEntry> updatedLe =
                std::move(entry.mLedgerEntry).releaseFromScope(*this);
            if (updatedLe)
            {
                // Update the entry without loading it from the ltx, and use
                // `mIsNew` flag instead to distinguish between init and live
                // entries.
                if (entry.mIsNew)
                {
                    ltx.createWithoutLoading(
                        InternalLedgerEntry(std::move(*updatedLe)));
                }
                else
                {
                    ltx.updateWithoutLoading(
                        InternalLedgerEntry(std::move(*updatedLe)));
                }
            }
            else if (!entry.mIsNew)
            {
                ltx.erase(key.ledgerKey());
            }
            // An entry that was both created and deleted during this phase
            // (nullopt value with mIsNew set) never reaches the ltx at all.
        }
    }

    // While the final state of a restored key that will be written to the
    // Live BucketList is already handled in mGlobalEntryMap, we need to
    // let the ltx know what keys were restored so that:
    // 1. Hot Archive restores can be removed from the Hot Archive BucketList
    // 2. The ArchivedStateConsistency invariant can validate both hot archive
    //    and live BucketList restores
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
    // Live BucketList restores are only tracked in LedgerTxn for the
    // ArchivedStateConsistency invariant, but we unconditionally track it for
    // now.
    for (auto const& kvp : mGlobalRestoredEntries.liveBucketList)
    {
        if (kvp.first.type() != TTL)
        {
            auto it = mGlobalRestoredEntries.liveBucketList.find(
                getTTLKey(kvp.first));
            releaseAssertOrThrow(it !=
                                 mGlobalRestoredEntries.liveBucketList.end());
            ltx.markRestoredFromLiveBucketList(kvp.second, it->second);
        }
    }
}

uint32_t
GlobalParallelApplyLedgerState::getSnapshotLedgerSeq() const
{
    return mInMemorySorobanState.getLedgerSeq();
}

size_t
GlobalParallelApplyLedgerState::globalMapShardOf(
    ParallelApplyLedgerKey const& key) const
{
    // Truncate the low bits of the hash to avoid clumping of keys in per-shard
    // maps in case if the shard count divides the map size.
    return (key.hash() >> 24) % mGlobalEntryMap.size();
}

ParallelApplyEntryMap<GlobalParallelApplyLedgerState::staticScope>&
GlobalParallelApplyLedgerState::globalMapShardFor(
    ParallelApplyLedgerKey const& key)
{
    return mGlobalEntryMap[globalMapShardOf(key)];
}

GlobalParallelApplyEntry const*
GlobalParallelApplyLedgerState::findInGlobalEntryMap(
    ParallelApplyLedgerKey const& key) const
{
    auto const& shard = mGlobalEntryMap[globalMapShardOf(key)];
    auto it = shard.find(key);
    return it == shard.end() ? nullptr : &it->second;
}

RestoredEntries const&
GlobalParallelApplyLedgerState::getRestoredEntries() const
{
    return mGlobalRestoredEntries;
}

bool
GlobalParallelApplyLedgerState::maybeMergeRoTTLBumps(
    ParallelApplyLedgerKey const& key, GlobalParallelApplyEntry const& newEntry,
    GlobalParallelApplyEntry& oldEntry,
    ParallelApplyLedgerKeySet const& readWriteSet)
{
    // Read Only bumps will always be updating a pre-existing value. TTL
    // creation (!oldEntry) or deletion (!newEntry) are write conflicts that
    // don't have merge special casing.
    std::optional<LedgerEntry> const& newLe =
        newEntry.mLedgerEntry.readInScope(*this);
    auto merged = false;
    oldEntry.mLedgerEntry.modifyInScope(
        *this, [&](std::optional<LedgerEntry>& oldLe) {
            if (newLe && oldLe && key.ledgerKey().type() == TTL)
            {
                releaseAssertOrThrow(newLe.value().data.type() == TTL);
                releaseAssertOrThrow(oldLe.value().data.type() == TTL);
                if (readWriteSet.find(key) == readWriteSet.end())
                {
                    uint32_t const& newTTL = ttl(newLe);
                    uint32_t& oldTTL = ttl(oldLe);
                    oldTTL = std::max(oldTTL, newTTL);
                    merged = true;
                }
            }
        });
    return merged;
}

void
GlobalParallelApplyLedgerState::commitChangeFromThread(
    ThreadParallelApplyLedgerState const& thread,
    ParallelApplyLedgerKey const& key, ThreadParallelApplyEntry&& parEntry,
    ParallelApplyLedgerKeySet const& readWriteSet)
{
    if (!parEntry.mIsDirty)
    {
        return;
    }
    auto rescopedParEntry = std::move(parEntry).rescope(thread, *this);
    auto& shard = globalMapShardFor(key);
    auto it = shard.find(key);
    if (it == shard.end())
    {
        shard.emplace(key, std::move(rescopedParEntry));
    }
    else if (!maybeMergeRoTTLBumps(key, rescopedParEntry, it->second,
                                   readWriteSet))
    {
        // mIsNew is relative to the pre-phase ledger state, so the flag
        // recorded when the entry was first tracked stays authoritative.
        rescopedParEntry.mIsNew = it->second.mIsNew;
        it->second = std::move(rescopedParEntry);
    }
}

void
GlobalParallelApplyLedgerState::commitShardChangesFromThreads(
    size_t shardIdx,
    std::vector<std::unique_ptr<ThreadParallelApplyLedgerState>> const& threads,
    size_t rwKeyCountHint)
{
    ZoneScoped;
    // Subset of the stage's read-write TTL key set belonging to this shard.
    ParallelApplyLedgerKeySet readWriteSet;
    readWriteSet.reserve(rwKeyCountHint / mGlobalEntryMap.size() + 1);
    for (auto const& thread : threads)
    {
        for (auto const& key : thread->getRwFootprintTTLKeys())
        {
            if (globalMapShardOf(key) == shardIdx)
            {
                readWriteSet.emplace(key);
            }
        }
    }

    for (auto const& thread : threads)
    {
        for (auto& [key, entry] : thread->getEntryMapMut())
        {
            if (globalMapShardOf(key) != shardIdx)
            {
                continue;
            }
            // Move the shard's entries out of the thread's map into the global
            // map. This is safe thanks to the sharding invariant: each worker
            // thread only modifies the disjoint set of values belonging to its
            // shard, and the map structure itself is not modified by any
            // thread.
            commitChangeFromThread(*thread, key, std::move(entry),
                                   readWriteSet);
        }
    }
}

void
GlobalParallelApplyLedgerState::commitChangesFromThreads(
    AppConnector& app,
    std::vector<std::unique_ptr<ThreadParallelApplyLedgerState>> const& threads)
{
    ZoneScoped;
    releaseAssert(threadIsMain() ||
                  app.threadIsType(Application::ThreadType::APPLY));

    for (auto const& thread : threads)
    {
        thread->scopeDeactivate();
    }

    // Size hint for estimating per-worker read-write TTL key count.
    size_t rwKeyCountHint = 0;
    for (auto const& thread : threads)
    {
        rwKeyCountHint += thread->getRwFootprintTTLKeys().size();
    }

    // Merge the per-thread maps on parallel workers, one per global-map shard.
    // Every worker scans every thread map but only merges the entries in its
    // own shard, so the workers write to disjoint entries and need no
    // synchronization.
    size_t shardCount = mGlobalEntryMap.size();
    std::vector<std::function<int()>> tasks;
    tasks.reserve(shardCount);
    for (size_t i = 0; i < shardCount; ++i)
    {
        tasks.emplace_back([this, i, &threads, rwKeyCountHint]() {
            commitShardChangesFromThreads(i, threads, rwKeyCountHint);
            return 0;
        });
    }
    app.getBatchExecutor().executeBatch(std::move(tasks));

    // The restored entry sets are not sharded, so we need to merge them
    // sequentially.
    for (auto const& thread : threads)
    {
        mGlobalRestoredEntries.addRestoresFrom(thread->getRestoredEntries());
    }
}

void
ThreadParallelApplyLedgerState::collectClusterFootprintEntriesFromGlobal(
    AppConnector& app, GlobalParallelApplyLedgerState const& global,
    Cluster const& cluster)
{
    // As part of the initialization of this thread state, we need to
    // collect all the keys that are in the global state map. For any keys
    // we need not in the global state, we will fetch them from the live
    // applyView, in memory soroban state, or the hot archive later.
    auto fetchFromGlobal = [&](ParallelApplyLedgerKey const& key) {
        if (mThreadEntryMap.find(key) != mThreadEntryMap.end())
        {
            return;
        }

        auto const* globalEntry = global.findInGlobalEntryMap(key);
        if (globalEntry != nullptr)
        {
            mThreadEntryMap.emplace(
                key,
                ThreadParallelApplyEntry::clean(
                    scopeAdoptEntryOptFrom(globalEntry->mLedgerEntry, global),
                    globalEntry->mIsNew));
        }
    };

    auto collectFootprint = [&](xdr::xvector<LedgerKey> const& keys,
                                bool isReadWrite) {
        for (auto const& key : keys)
        {
            fetchFromGlobal(ParallelApplyLedgerKey(key));
            if (isSorobanEntry(key))
            {
                ParallelApplyLedgerKey ttlKey(getTTLKey(key));
                fetchFromGlobal(ttlKey);
                if (isReadWrite)
                {
                    mRwFootprintTTLKeys.push_back(std::move(ttlKey));
                }
            }
        }
    };

    // Reserve the max possible size for mRwFootprintTTLKeys once for the whole
    // cluster.
    size_t rwKeyCount = 0;
    for (auto const& txBundle : cluster)
    {
        rwKeyCount +=
            txBundle.getTx()->sorobanResources().footprint.readWrite.size();
    }
    mRwFootprintTTLKeys.reserve(rwKeyCount);

    for (auto const& txBundle : cluster)
    {
        auto const& footprint = txBundle.getTx()->sorobanResources().footprint;
        collectFootprint(footprint.readWrite, /*isReadWrite=*/true);
        collectFootprint(footprint.readOnly, /*isReadWrite=*/false);
    }
}

ThreadParallelApplyLedgerState::ThreadParallelApplyLedgerState(
    AppConnector& app, GlobalParallelApplyLedgerState const& global,
    Cluster const& cluster, size_t clusterIdx)
    : LedgerEntryScope(ScopeIdT(clusterIdx, global.mScopeID.mLedger))
    , mLCLApplyView(global.mLCLApplyView)
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

        ParallelApplyLedgerKey ttlKey(getTTLKey(lk));
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
                                getSnapshotLedgerSeq() + 1, /*isNew=*/false);
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
                                getSnapshotLedgerSeq() + 1, /*isNew=*/false);
                }
            });
    }
}

std::vector<ParallelApplyLedgerKey> const&
ThreadParallelApplyLedgerState::getRwFootprintTTLKeys() const
{
    return mRwFootprintTTLKeys;
}

ThreadParallelApplyEntryMap&
ThreadParallelApplyLedgerState::getEntryMapMut()
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
    return getLiveEntryOpt(ParallelApplyLedgerKey(key));
}

ThreadParallelApplyLedgerState::OptionalEntryT
ThreadParallelApplyLedgerState::getLiveEntryOpt(
    ParallelApplyLedgerKey const& key) const
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
    // memory soroban state or the live applyView.

    // Check InMemorySorobanState cache for soroban types
    std::shared_ptr<LedgerEntry const> res;
    if (InMemorySorobanState::isInMemoryType(key))
    {
        res = mInMemorySorobanState.get(key);
    }
    else
    {
        res = mLCLApplyView.loadLiveEntry(key);
    }

    return scopeAdoptEntryOpt(res ? std::make_optional(*res) : std::nullopt);
}

void
ThreadParallelApplyLedgerState::putEntry(ParallelApplyLedgerKey const& key,
                                         ThreadParallelApplyEntry&& entry)
{
    auto [it, inserted] = mThreadEntryMap.try_emplace(key, std::move(entry));
    if (!inserted)
    {
        // mIsNew is relative to the pre-phase ledger state, so the flag
        // recorded when the entry was first tracked stays authoritative.
        // NB: try_emplace does not move from its arguments when the key
        // already exists.
        entry.mIsNew = it->second.mIsNew;
        it->second = std::move(entry);
    }
}

void
ThreadParallelApplyLedgerState::upsertEntry(ParallelApplyLedgerKey const& key,
                                            ThreadParApplyLedgerEntry&& entry,
                                            uint32_t ledgerSeq, bool isNew)
{
    auto parAppEntry = ThreadParallelApplyEntry::dirty(std::move(entry), isNew);
    parAppEntry.mLedgerEntry.modifyInScope(
        *this, [&](std::optional<LedgerEntry>& le) {
            releaseAssertOrThrow(le);
            le.value().lastModifiedLedgerSeq = ledgerSeq;
        });
    putEntry(key, std::move(parAppEntry));
}

void
ThreadParallelApplyLedgerState::eraseEntry(ParallelApplyLedgerKey const& key,
                                           bool isNew)
{
    putEntry(key, ThreadParallelApplyEntry::dirty(
                      scopeAdoptEntryOpt(std::nullopt), isNew));
}

void
ThreadParallelApplyLedgerState::commitChangeFromSuccessfulTx(
    ParallelApplyLedgerKey const& key,
    ThreadParApplyLedgerEntryOpt&& newScopedEntryOpt,
    ParallelApplyLedgerKeySet const& roTTLSet)
{
    // We need to make a read-only lookup of the entry corresponding to the key,
    // but `getLiveEntryOpt` always copies the entry, even when it's already
    // available in the thread entry map. So only call it if the entry is not
    // in the map (and thus is copied from the snapshot).
    auto it = mThreadEntryMap.find(key);
    bool isInMap = it != mThreadEntryMap.end();
    std::optional<ThreadParApplyLedgerEntryOpt> oldScopedEntryCopyOpt;
    if (!isInMap)
    {
        oldScopedEntryCopyOpt.emplace(getLiveEntryOpt(key));
    }
    std::optional<LedgerEntry> const& oldEntryOpt =
        isInMap ? it->second.mLedgerEntry.readInScope(*this)
                : oldScopedEntryCopyOpt->readInScope(*this);
    bool isNew = isInMap ? it->second.mIsNew : !oldEntryOpt.has_value();
    std::optional<LedgerEntry> const& newEntryOpt =
        newScopedEntryOpt.readInScope(*this);

    if (newEntryOpt && oldEntryOpt && roTTLSet.find(key) != roTTLSet.end())
    {
        auto const& entry = newEntryOpt.value();
        // Accumulate RO bumps instead of writing them to the entryMap.
        releaseAssertOrThrow(ttl(entry) >= ttl(oldEntryOpt.value()));
        updateMaxOfRoTTLBump(mRoTTLBumps, key, entry);
    }
    else if (newEntryOpt)
    {
        auto newLe = std::move(newScopedEntryOpt).releaseFromScope(*this);
        upsertEntry(key, scopeAdoptEntry(std::move(newLe.value())),
                    getSnapshotLedgerSeq() + 1, isNew);
    }
    else
    {
        eraseEntry(key, isNew);
    }
}

void
ThreadParallelApplyLedgerState::setDeltaForInvariantsFromSuccessfulTx(
    ParallelTxSuccessVal const& res, TxEffects& effects) const
{
    ZoneScoped;
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
            // If the entry was not found in the live applyView, we check if it
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
        effects.setDeltaEntryForInvariants(lk, entryDelta);
    }
}

void
ThreadParallelApplyLedgerState::commitChangesFromSuccessfulTx(
    ParallelTxSuccessVal&& res, TxBundle const& txBundle)
{
    auto roTTLSet = buildRoTTLSet(txBundle);
    for (auto& [key, txScopedEntryOpt] : res.getModifiedEntryMapMut())
    {
        commitChangeFromSuccessfulTx(
            key, scopeAdoptEntryOptFrom(std::move(txScopedEntryOpt), res),
            roTTLSet);
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
    return mInMemorySorobanState.getLedgerSeq();
}

SorobanNetworkConfig const&
ThreadParallelApplyLedgerState::getSorobanConfig() const
{
    return mSorobanConfig;
}

ApplyLedgerView const&
ThreadParallelApplyLedgerState::getSnapshot() const
{
    return mLCLApplyView;
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
    return getLiveEntryOpt(ParallelApplyLedgerKey(key));
}

TxParallelApplyLedgerState::OptionalEntryT
TxParallelApplyLedgerState::getLiveEntryOpt(
    ParallelApplyLedgerKey const& key) const
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
    //  1. The entry exists in the parent maps (thread state or live applyView)
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

    ParallelApplyLedgerKey pk(key);
    bool liveEntryExistedAlready =
        getLiveEntryOpt(pk).readInScope(*this).has_value();
    CLOG_TRACE(Tx, "parallel apply thread {} upserting {} key {}",
               std::this_thread::get_id(),
               liveEntryExistedAlready ? "already-live" : "new",
               xdr::xdr_to_string(key, "key"));

    auto [mapEntry, _] =
        mTxEntryMap.insert_or_assign(std::move(pk), scopeAdoptEntryOpt(entry));
    mapEntry->second.modifyInScope(*this, [&](std::optional<LedgerEntry>& le) {
        releaseAssertOrThrow(le);
        le.value().lastModifiedLedgerSeq = ledgerSeq;
    });
    return !liveEntryExistedAlready;
}

bool
TxParallelApplyLedgerState::eraseEntryIfExists(LedgerKey const& key)
{
    ParallelApplyLedgerKey pk(key);
    bool liveEntryExistedAlready =
        getLiveEntryOpt(pk).readInScope(*this).has_value();
    if (liveEntryExistedAlready)
    {
        // NB: we only erase an entry if it doesn't already exist in
        // parents (thread state or live applyView), otherwise
        // we will produce mismatched erases that don't relate to
        // any pre-state key when calculating the ledger delta.
        CLOG_TRACE(Tx, "parallel apply thread {} erasing {}",
                   std::this_thread::get_id(), xdr::xdr_to_string(key, "key"));
        mTxEntryMap.insert_or_assign(std::move(pk),
                                     scopeAdoptEntryOpt(std::nullopt));
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

std::optional<ParallelTxSuccessVal>
TxParallelApplyLedgerState::takeResult(bool success)
{
    if (success)
    {
        CLOG_TRACE(Tx,
                   "parallel apply thread {} succeeded with {} dirty entries",
                   std::this_thread::get_id(), mTxEntryMap.size());
        return ParallelTxSuccessVal{std::move(mTxEntryMap),
                                    std::move(mTxRestoredEntries), mScopeID};
    }
    else
    {
        CLOG_TRACE(Tx, "parallel apply thread {} failed with {} dirty entries",
                   std::this_thread::get_id(), mTxEntryMap.size());
        return std::nullopt;
    }
}

uint32_t
TxParallelApplyLedgerState::getSnapshotLedgerSeq() const
{
    return mThreadState.getSnapshotLedgerSeq();
}
}
