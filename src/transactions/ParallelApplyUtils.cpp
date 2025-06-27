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
#include "xdr/Stellar-ledger-entries.h"
#include "xdrpp/printer.h"
#include <fmt/core.h>
#include <fmt/std.h>
#include <thread>

namespace stellar
{

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

void
preParallelApplyAndCollectModifiedClassicEntries(
    AppConnector& app, AbstractLedgerTxn& ltx,
    std::vector<ApplyStage> const& stages,
    ParallelApplyEntryMap& globalEntryMap)
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
            txBundle.getTx()->preParallelApply(app, ltx,
                                               txBundle.getEffects().getMeta(),
                                               txBundle.getResPayload());
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

void
writeDirtyThreadEntryMapEntriesToGlobalEntryMap(
    ParallelApplyEntryMap const& threadEntryMap,
    ParallelApplyEntryMap& globalEntryMap,
    std::unordered_set<LedgerKey> const& isInReadWriteSet)
{
    for (auto const& entry : threadEntryMap)
    {
        // Only update if dirty bit is set
        if (!entry.second.mIsDirty)
        {
            continue;
        }

        if (entry.second.mLedgerEntry)
        {
            auto const& updatedEntry = *entry.second.mLedgerEntry;

            auto it = globalEntryMap.find(entry.first);
            if (it != globalEntryMap.end() && it->second.mLedgerEntry)
            {
                auto const& currentEntry = *it->second.mLedgerEntry;
                if (currentEntry.data.type() == TTL)
                {
                    auto currLiveUntil =
                        currentEntry.data.ttl().liveUntilLedgerSeq;
                    auto newLiveUntil =
                        updatedEntry.data.ttl().liveUntilLedgerSeq;

                    // The only scenario where we accept a reduction in TTL
                    // is one where an entry was deleted, and then
                    // recreated. This can only happen if the key was in the
                    // readWrite set, so if it's not then this is just a
                    // parallel readOnly bump that we can ignore here.
                    if (newLiveUntil <= currLiveUntil &&
                        isInReadWriteSet.count(entry.first) == 0)
                    {
                        continue;
                    }
                }
                it->second = entry.second;
            }
            else
            {
                if (it != globalEntryMap.end())
                {
                    it->second = entry.second;
                }
                else
                {
                    globalEntryMap.emplace(
                        entry.first,
                        ParallelApplyEntry::dirtyLive(updatedEntry));
                }
            }
        }
        else
        {
            auto dead = ParallelApplyEntry::dirtyDead();
            auto [it, inserted] = globalEntryMap.emplace(entry.first, dead);
            if (!inserted)
            {
                it->second = dead;
            }
        }
    }
}

void
writeDirtyMapEntriesToGlobalEntryMap(
    std::vector<std::unique_ptr<ParallelApplyEntryMap>> const&
        entryMapsByCluster,
    ParallelApplyEntryMap& globalEntryMap,
    std::unordered_set<LedgerKey> const& isInReadWriteSet)
{
    // merge entryMaps into globalEntryMap
    for (auto const& map : entryMapsByCluster)
    {
        writeDirtyThreadEntryMapEntriesToGlobalEntryMap(*map, globalEntryMap,
                                                        isInReadWriteSet);
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

// For every soroban LE in `txBundle`s RW footprint, ensure we've flushed any
// buffered RO TTL bumps stored in `roTTLBumps` to the `entryMap`.
//
// We do so because this tx that does an RW access to the LE:
//
//   - _Will_ be clustered with all other RO and RW txs touching the LE, so we
//     don't need to worry about other clusters touching this LE or bumping its
//     TTL in parallel. This LE and its TTL are sequentialized in this cluster.
//
//   - _Might_ be clustered with an earlier tx that did an RO TTL bump of the
//     LE, which could have changed the cost of the LE write happening in this
//     tx. We do have to worry about that!
//
// So: for correct accounting of the write happening in this tx, we have to
// flush any pending RO TTL bumps that interfere with its RW footprint.
void
flushRoTTLBumpsRequiredByTx(SearchableSnapshotConstPtr liveSnapshot,
                            ParallelApplyEntryMap& entryMap,
                            UnorderedMap<LedgerKey, uint32_t>& roTTLBumps,
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

        auto const& ttlKey = getTTLKey(lk);
        auto b = roTTLBumps.find(ttlKey);
        if (b != roTTLBumps.end())
        {
            // If we have residual RO TTL bumps for this key,
            // the entry must exist. If it was deleted, we would've
            // erased the TTL key from roTTLBumps.

            // "Commit" max RO bump now that the key is in a
            // readWrite set
            auto e = entryMap.find(ttlKey);
            if (e != entryMap.end())
            {
                releaseAssert(e->second.mLedgerEntry);
                releaseAssert(ttl(e->second.mLedgerEntry) <= b->second);

                ttl(e->second.mLedgerEntry) = b->second;

                // Mark as dirty so this entry gets written.
                e->second.mIsDirty = true;
            }
            else
            {
                // The entry being must be in the liveSnapshot
                // if it is not in the entryMap.
                auto snapshotEntry = liveSnapshot->load(ttlKey);
                releaseAssert(snapshotEntry);

                auto le = *snapshotEntry;
                releaseAssert(ttl(le) <= b->second);

                ttl(le) = b->second;
                entryMap.emplace(ttlKey, ParallelApplyEntry{le, true});
            }
            roTTLBumps.erase(b);
        }
    }
}

// Ensure that for each remaining RO TTL bump in `roTTLBumps`, the
// TTL entry is present in the `entryMap` and is >= the bump TTL.
void
flushResidualRoTTLBumps(SearchableSnapshotConstPtr liveSnapshot,
                        ParallelApplyEntryMap& entryMap,
                        UnorderedMap<LedgerKey, uint32_t> const& roTTLBumps)
{
    for (auto const& [lk, ttlBump] : roTTLBumps)
    {
        auto entryOpt = getLiveEntry(lk, liveSnapshot, entryMap);

        // The entry should always exist. If the entry was deleted,
        // then we would've erased the TTL key from roTTLBumps.
        releaseAssert(entryOpt);
        if (ttl(entryOpt) < ttlBump)
        {
            auto it = entryMap.find(lk);
            if (it == entryMap.end())
            {
                ttl(entryOpt) = ttlBump;
                entryMap.emplace(lk, ParallelApplyEntry{*entryOpt, true});
            }
            else
            {
                ttl(it->second.mLedgerEntry) = ttlBump;
                it->second.mIsDirty = true;
            }
        }
    }
}

// Construct a map of all the TTL keys associated with all soroban
// (code-or-data) keys named in the footprint of the `txBundle`, along with a
// boolean indicating whether they're RO TTL keys. When false, they're RW.
UnorderedMap<LedgerKey, bool>
buildRoTTLMap(TxBundle const& txBundle)
{
    UnorderedMap<LedgerKey, bool> isReadOnlyTTLMap;
    for (auto const& ro :
         txBundle.getTx()->sorobanResources().footprint.readOnly)
    {
        if (!isSorobanEntry(ro))
        {
            continue;
        }
        isReadOnlyTTLMap.emplace(getTTLKey(ro), true);
    }
    for (auto const& rw :
         txBundle.getTx()->sorobanResources().footprint.readWrite)
    {
        if (!isSorobanEntry(rw))
        {
            continue;
        }
        isReadOnlyTTLMap.emplace(getTTLKey(rw), false);
    }

    return isReadOnlyTTLMap;
}

// Look up a key in the RO TTL map built above. If the key is either not a TTL,
// or not RO, return false.
bool
isRoTTLKey(UnorderedMap<LedgerKey, bool> const& rmap, LedgerKey const& lk)
{
    auto it = rmap.find(lk);
    releaseAssert(lk.type() == TTL || it == rmap.end());
    return lk.type() == TTL && it->second;
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

// Writes the entries in `res` back to the `entryMap`, `roTTLBumps` and
// `threadRestoredEntries` variables that are accumulating the state of
// the transaction cluster.
void
recordModifiedAndRestoredEntries(SearchableSnapshotConstPtr liveSnapshot,
                                 ParallelApplyEntryMap& entryMap,
                                 UnorderedMap<LedgerKey, uint32_t>& roTTLBumps,
                                 RestoredEntries& threadRestoredEntries,
                                 TxBundle const& txBundle,
                                 ParallelTxReturnVal const& res)
{
    releaseAssert(res.getSuccess());
    auto rmap = buildRoTTLMap(txBundle);

    // now apply the entry changes to entryMap
    for (auto const& [lk, updatedLe] : res.getModifiedEntryMap())
    {
        auto oldEntryOpt = getLiveEntry(lk, liveSnapshot, entryMap);

        if (isRoTTLKey(rmap, lk) && oldEntryOpt && updatedLe)
        {
            // Accumulate RO bumps instead of writing them to the entryMap.
            releaseAssert(ttl(updatedLe) >= ttl(oldEntryOpt));
            updateMaxOfRoTTLBump(roTTLBumps, lk, updatedLe);
        }
        else
        {
            // A entry deletion will be marked by a nullopt le.
            // Set the dirty bit so it'll be written to ltx later.
            auto e = ParallelApplyEntry{updatedLe, true};
            auto [it, inserted] = entryMap.emplace(lk, e);
            if (!inserted)
            {
                it->second = e;
            }
        }
    }
    threadRestoredEntries.addRestoresFrom(res.getRestoredEntries());
}

void
setDelta(SearchableSnapshotConstPtr liveSnapshot,
         ParallelApplyEntryMap const& entryMap,
         OpModifiedEntryMap const& opModifiedEntryMap,
         UnorderedMap<LedgerKey, LedgerEntry> const& hotArchiveRestores,
         ParallelLedgerInfo const& ledgerInfo, TxEffects& effects)
{
    for (auto const& newUpdates : opModifiedEntryMap)
    {
        auto const& lk = newUpdates.first;
        auto const& le = newUpdates.second;

        auto prevLe = getLiveEntry(lk, liveSnapshot, entryMap);

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
            auto it = hotArchiveRestores.find(lk);
            if (it != hotArchiveRestores.end())
            {
                entryDelta.previous =
                    std::make_shared<InternalLedgerEntry>(it->second);
            }
        }

        if (le)
        {
            auto deltaLe = *le;
            // This is for the invariants check in LedgerManager
            deltaLe.lastModifiedLedgerSeq = ledgerInfo.getLedgerSeq();

            entryDelta.current = std::make_shared<InternalLedgerEntry>(deltaLe);
        }
        releaseAssertOrThrow(entryDelta.current || entryDelta.previous);
        effects.setDeltaEntry(lk, entryDelta);
    }
}

std::optional<LedgerEntry>
getLiveEntry(LedgerKey const& lk, SearchableSnapshotConstPtr liveSnapshot,
             ParallelApplyEntryMap const& entryMap)
{
    // TODO: These copies aren't ideal.
    auto entryIter = entryMap.find(lk);
    if (entryIter != entryMap.end())
    {
        return entryIter->second.mLedgerEntry;
    }
    else
    {
        auto res = liveSnapshot->load(lk);
        return res ? std::make_optional(*res) : std::nullopt;
    }
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
    ParallelLedgerInfo const& ledgerInfo,
    SearchableSnapshotConstPtr liveSnapshot)
    : mLedgerInfo(ledgerInfo), mOpState(threadState)
{
}

std::optional<LedgerEntry>
ParallelLedgerAccessHelper::getLedgerEntryOpt(LedgerKey const& key)
{
    return mOpState.getLiveEntryOpt(key);
}

uint32_t
ParallelLedgerAccessHelper::getLedgerSeq()
{
    return mLedgerInfo.getLedgerSeq();
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
    return mOpState.upsertEntry(key, entry);
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
    std::vector<ApplyStage> const& stages)
    : mHotArchiveSnapshot(app.copySearchableHotArchiveBucketListSnapshot())
    , mLiveSnapshot(app.copySearchableLiveBucketListSnapshot())
{
    // From now on, we will be using globalState, liveSnapshots, and the
    // hotArchive to collect all entries. Before we continue though, we need to
    // load into the globalEntryMap any classic entries that have been modified
    // in this ledger because those changes won't be reflected in the
    // globalEntryMap. The entries that could've changed are accounts and
    // trustlines from the classic phase, as well as fee source accounts that
    // had their sequence numbers bumped and fees charged. preParallelApply will
    // update sequence numbers so it needs to be called before we check
    // LedgerTxn.
    preParallelApplyAndCollectModifiedClassicEntries(app, ltx, stages,
                                                     mGlobalEntryMap);
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
            releaseAssert(it != mGlobalRestoredEntries.hotArchive.end());
            ltxInner.markRestoredFromHotArchive(kvp.second, it->second);
        }
    }
    ltxInner.commit();
}

std::optional<LedgerEntry>
GlobalParallelApplyLedgerState::getLiveEntryOpt(LedgerKey const& key) const
{
    auto it0 = mGlobalEntryMap.find(key);
    if (it0 != mGlobalEntryMap.end())
    {
        return it0->second.mLedgerEntry;
    }
    // Invariant check: if an entry is not in mGlobalEntryMap it should also not
    // be in the restored map.
    releaseAssert(!mGlobalRestoredEntries.entryWasRestored(key));
    auto res = mLiveSnapshot->load(key);
    return res ? std::make_optional(*res) : std::nullopt;
}

void
GlobalParallelApplyLedgerState::upsertEntry(LedgerKey const& key,
                                            LedgerEntry const& entry)
{
    auto e = ParallelApplyEntry::dirtyLive(entry);
    auto [it, inserted] = mGlobalEntryMap.emplace(key, e);
    if (!inserted)
    {
        it->second = e;
    }
}

void
GlobalParallelApplyLedgerState::eraseEntry(LedgerKey const& key)
{
    auto e = ParallelApplyEntry::dirtyDead();
    auto [it, inserted] = mGlobalEntryMap.emplace(key, e);
    if (!inserted)
    {
        it->second = e;
    }
}

RestoredEntries const&
GlobalParallelApplyLedgerState::getRestoredEntries() const
{
    return mGlobalRestoredEntries;
}

void
GlobalParallelApplyLedgerState::commitChangesFromThread(
    AppConnector& app, ThreadParallelApplyLedgerState const& thread,
    ApplyStage const& stage)
{
    releaseAssert(threadIsMain() ||
                  app.threadIsType(Application::ThreadType::APPLY));
    auto readWriteSet = getReadWriteKeysForStage(stage);
    writeDirtyThreadEntryMapEntriesToGlobalEntryMap(
        thread.getEntryMap(), mGlobalEntryMap, readWriteSet);
    mGlobalRestoredEntries.addRestoresFrom(thread.getRestoredEntries());
}

void
GlobalParallelApplyLedgerState::commitChangesFromThreads(
    AppConnector& app,
    std::vector<std::unique_ptr<ThreadParallelApplyLedgerState>> const& threads,
    ApplyStage const& stage)
{
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
    // This loop is similar to "collectEntries" in the previous
    // code, except that:
    //
    //  - it skips copying duplicates if 2 txs touch the same key; just an
    //    optimization.
    //
    //  - it uses parent.getLiveEntryOpt which will:
    //
    //    - check a few more invariants than the old getLiveEntry.
    //
    //    - also copy footprint entries from the live snapshot -- not just
    //      propagated/dirty stuff from the parent entry map, so it's a bit
    //      of a pre-load (though not a pre-hot-restore)

    for (auto const& txBundle : cluster)
    {
        auto const& footprint = txBundle.getTx()->sorobanResources().footprint;
        for (auto const& keys : {footprint.readWrite, footprint.readOnly})
        {
            for (auto const& key : keys)
            {
                if (mThreadEntryMap.find(key) != mThreadEntryMap.end())
                {
                    continue;
                }
                auto opt = global.getLiveEntryOpt(key);
                if (opt)
                {
                    mThreadEntryMap.emplace(
                        key, ParallelApplyEntry::cleanLive(opt.value()));
                    if (isSorobanEntry(key))
                    {
                        auto ttlKey = getTTLKey(key);
                        auto ttlOpt = global.getLiveEntryOpt(ttlKey);
                        releaseAssertOrThrow(ttlOpt);
                        mThreadEntryMap.emplace(
                            ttlKey,
                            ParallelApplyEntry::cleanLive(ttlOpt.value()));
                    }
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
{
    mPreviouslyRestoredEntries.addRestoresFrom(global.getRestoredEntries());
    collectClusterFootprintEntriesFromGlobal(app, global, cluster);
}

void
ThreadParallelApplyLedgerState::flushRoTTLBumpsInTxWriteFootprint(
    const TxBundle& txBundle)
{

    flushRoTTLBumpsRequiredByTx(mLiveSnapshot, mThreadEntryMap, mRoTTLBumps,
                                txBundle);
}

void
ThreadParallelApplyLedgerState::flushRemainingRoTTLBumps()
{
    flushResidualRoTTLBumps(mLiveSnapshot, mThreadEntryMap, mRoTTLBumps);
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
    // Invariant check: if an entry is not in mThreadEntryMap it should also not
    // be in the restored map, unless it's a TTL entry, in which case only a
    // weaker invariant holds: it can't be in the _hot_ restored map. It's
    // possible for a TTL entry to be just in the live restored map if its
    // associated entry was live-restored.
    if (key.type() == TTL)
    {
        releaseAssert(!mThreadRestoredEntries.entryWasRestoredFromMap(
            key, mThreadRestoredEntries.hotArchive));
    }
    else
    {
        releaseAssert(!mThreadRestoredEntries.entryWasRestored(key));
    }
    auto res = mLiveSnapshot->load(key);
    return res ? std::make_optional(*res) : std::nullopt;
}

void
ThreadParallelApplyLedgerState::commitChangesFromSuccessfulOp(
    ParallelTxReturnVal const& res, TxBundle const& txBundle)
{
    releaseAssert(res.getSuccess());
    recordModifiedAndRestoredEntries(mLiveSnapshot, mThreadEntryMap,
                                     mRoTTLBumps, mThreadRestoredEntries,
                                     txBundle, res);
}

bool
ThreadParallelApplyLedgerState::entryWasRestored(LedgerKey const& key) const
{
    return mThreadRestoredEntries.entryWasRestored(key) ||
           mPreviouslyRestoredEntries.entryWasRestored(key);
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
                                        LedgerEntry const& entry)
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
    auto [it, inserted] = mOpEntryMap.emplace(key, entry);
    if (!inserted)
    {
        it->second = entry;
    }
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
        auto [it, inserted] = mOpEntryMap.emplace(key, std::nullopt);
        if (!inserted)
        {
            it->second = std::nullopt;
        }
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

}