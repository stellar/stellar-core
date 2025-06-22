// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/ParallelApplyUtils.h"
#include "bucket/BucketSnapshotManager.h"
#include "bucket/BucketUtils.h"
#include "ledger/NetworkConfig.h"
#include "transactions/MutableTransactionResult.h"
#include "transactions/TransactionFrameBase.h"
#include <fmt/core.h>

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

std::unique_ptr<ParallelApplyEntryMap>
collectEntries(SearchableSnapshotConstPtr liveSnapshot,
               ParallelApplyEntryMap const& globalEntryMap,
               Cluster const& cluster)
{
    auto entryMap = std::make_unique<ParallelApplyEntryMap>();

    auto processKeys = [&](xdr::xvector<LedgerKey> const& keys) {
        for (auto const& lk : keys)
        {
            auto it = globalEntryMap.find(lk);
            if (it != globalEntryMap.end())
            {
                // If the entry exists, we take it
                entryMap->emplace(lk, it->second);
                if (isSorobanEntry(lk))
                {
                    // If it's a Soroban entry, we also add the TTL key
                    auto ttlKey = getTTLKey(lk);
                    auto ttlIt = globalEntryMap.find(ttlKey);
                    if (ttlIt != globalEntryMap.end())
                    {
                        // If the TTL entry exists, we take it
                        entryMap->emplace(ttlKey, ttlIt->second);
                    }
                }
            }
        }
    };

    for (auto const& txBundle : cluster)
    {
        auto const& footprint = txBundle.getTx()->sorobanResources().footprint;
        processKeys(footprint.readWrite);
        processKeys(footprint.readOnly);
    }

    return entryMap;
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
writeGlobalEntryMapToLedgerTxn(AbstractLedgerTxn& ltx,
                               ParallelApplyEntryMap const& globalEntryMap)
{
    LedgerTxn ltxInner(ltx);
    for (auto const& entry : globalEntryMap)
    {
        // Only update if dirty bit is set
        if (!entry.second.isDirty)
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
    ltxInner.commit();
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
        if (!entry.second.isDirty)
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
PreV23LedgerAccessHelper::eraseLedgerEntry(LedgerKey const& key)
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
    ParallelApplyEntryMap const& entryMap, ParallelLedgerInfo const& ledgerInfo,
    SearchableSnapshotConstPtr liveSnapshot)
    : mEntryMap(entryMap), mLedgerInfo(ledgerInfo), mLiveSnapshot(liveSnapshot)

{
}

std::optional<LedgerEntry>
ParallelLedgerAccessHelper::getLedgerEntryOpt(LedgerKey const& key)
{
    return getLiveEntry(key, mLiveSnapshot, mEntryMap);
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
    auto opEntryIter = mOpEntryMap.emplace(key, entry);
    // other methods can add restored entries to opEntryMap, so check if
    // we need to update the entry.
    if (opEntryIter.second == false)
    {
        opEntryIter.first->second = entry;
    }

    // Return true iff this key was created during this op.
    return !getLiveEntry(key, mLiveSnapshot, mEntryMap);
}

bool
ParallelLedgerAccessHelper::eraseLedgerEntry(LedgerKey const& key)
{
    // It's possible that the entry was restored, and put into
    // mOpEntryMap, so we need to check if it exists there first.

    auto it = mOpEntryMap.find(key);
    if (it != mOpEntryMap.end())
    {
        // If it exists, we mark it as a delete.
        it->second = std::nullopt;
        return true;
    }
    if (getLiveEntry(key, mLiveSnapshot, mEntryMap))
    {
        mOpEntryMap.emplace(key, std::nullopt);
        return true;
    }
    return false;
}

// We model the states in terms of a set of maps and snapshots. The
// relationships are subtle but basically follow an "newer information overrides
// older" pattern: per-op maps override per-thread maps which override the
// cross-thread "global" maps which override the bucket list snapshots. And of
// course when each newer type is successful it commits to its parent / older
// type.
//
// In this way the structure mirrors the ltx, but is not generalized to multiple
// "levels" and, crucially, has some special rules around _threading_. The
// per-thread objects retain no references at all to the global maps or
// snapshots, which are not threadsafe. Instead all information the per-thread
// maps will need is copied into the them when they're built, and only committed
// back to the parent once the threads using them are complete.
class ThreadParalllelApplyLedgerState;
class GlobalParallelApplyLedgerState
{

    // Contains the hot archive state from the start of the ledger close. If a
    // key is in here, it is "evicted". An invariant is that if a key is in here
    // it is _not_ in the live snapshot.
    SearchableHotArchiveSnapshotConstPtr mHotArchiveSnapshot;

    // Contains the live soroban state from the start of the ledger close. If a
    // key is in here, it is either "archived" or "live", depending on its TTL.
    // Classic entries are always live, soroban entries always have an
    // associated TTL entry and if the TTL is in the past the entry is
    // "archived", otherwise "live". An invariant is that if a key is in here it
    // is _not_ in the hot archive snapshot.
    SearchableSnapshotConstPtr mLiveSnapshot;

    // Contains restorations that happened during each stage of the parallel
    // soroban phase. As with mGlobalEntryMap, this is propagated stage to stage
    // by being split into per-thread maps and re-merged at the end of the
    // stage, before begin committed to the ltx at the end of the phase. As with
    // restorations inside the thread, these entries are the restored values _at
    // their time of restoration_ which may be further overridden by
    // mGlobalEntryMap.
    RestoredEntries mGlobalRestoredEntries;

    // Contains two different sets of entries:
    //
    //  - Classic entries modified during earlier sequential phases that are
    //    read during the parallel soroban phase. These are copied out of the
    //    ltx before the parallel soroban phase begins.
    //
    //  - Dirty entries resulting from each stage of the parallel soroban phase.
    //    These are propagated from stage to stage of the parallel soroban phase
    //    -- split into disjoint per-thread maps during execution and merged
    //    after -- as well as written back to the ltx at the phase's end.
    ParallelApplyEntryMap mGlobalEntryMap;

  public:
    GlobalParallelApplyLedgerState(AppConnector& app, AbstractLedgerTxn& ltx,
                                   std::vector<ApplyStage> const& stages)
        : mHotArchiveSnapshot(app.copySearchableHotArchiveBucketListSnapshot())
        , mLiveSnapshot(app.copySearchableLiveBucketListSnapshot())
    {
        preParallelApplyAndCollectModifiedClassicEntries(app, ltx, stages,
                                                         mGlobalEntryMap);
    }

    std::optional<LedgerEntry>
    getLiveEntryOpt(LedgerKey const& key) const
    {
        auto it0 = mGlobalEntryMap.find(key);
        if (it0 != mGlobalEntryMap.end())
        {
            return it0->second.mLedgerEntry;
        }
        auto rop = mGlobalRestoredEntries.getEntryOpt(key);
        if (rop)
        {
            return rop;
        }
        auto res = mLiveSnapshot->load(key);
        return res ? std::make_optional(*res) : std::nullopt;
    }

    // Returns true iff this upsert was a _creation_, relative to the
    // live entries visible.
    void
    upsertEntry(LedgerKey const& key, LedgerEntry const& entry)
    {
        auto e = ParallelApplyEntry::dirtyLive(entry);
        auto [it, inserted] = mGlobalEntryMap.emplace(key, e);
        if (!inserted)
        {
            it->second = e;
        }
    }

    // Returns true iff there was something live that got erased.
    void
    eraseEntry(LedgerKey const& key)
    {
        auto e = ParallelApplyEntry::dirtyDead();
        auto [it, inserted] = mGlobalEntryMap.emplace(key, e);
        if (!inserted)
        {
            it->second = e;
        }
    }

    friend class ThreadParallelApplyLedgerState;
};

class ThreadParallelApplyLedgerState
{
    // Copies of snapshots from the global state.
    SearchableHotArchiveSnapshotConstPtr mHotArchiveSnapshot;
    SearchableSnapshotConstPtr mLiveSnapshot;

    // Contains keys restored by any tx/op in the current thread's tx cluster
    // from the current stage of the parallel apply phase. Any entry should only
    // be in one of the two sub-maps here, live or hot. The entry in the map is
    // the entry value _at the time of restoration_ which may be overridden by
    // the entry map.
    RestoredEntries mThreadRestoredEntries;

    // Contains all entries accessed by any tx/op in the current thread's
    // tx cluster from the current stage of the parallel apply phase. As with
    // the live entry map, any soroban entry in here must have an associated TTL
    // entry.
    ParallelApplyEntryMap mThreadEntryMap;

  public:
    ThreadParallelApplyLedgerState(AppConnector& app,
                                   GlobalParallelApplyLedgerState const& global,
                                   Cluster const& cluster)
        // TODO: find a way to clone these from parent rather than asking the
        // snapshot manager again. That might have changed! NB taking a shared
        // pointer copy is not safe, the snapshot objects are not threadsafe.
        : mHotArchiveSnapshot(app.copySearchableHotArchiveBucketListSnapshot())
        , mLiveSnapshot(app.copySearchableLiveBucketListSnapshot())
    {
        releaseAssert(threadIsMain() ||
                      app.threadIsType(Application::ThreadType::APPLY));
        // Review note: this loop is similar to "collectEntries" in the previous
        // code, except that:
        //
        //  - it skips copying duplicates if 2 txs touch the same key, just an
        //    optimization.
        //
        //  - it uses parent.getLiveEntryOpt which will:
        //
        //    - copy entries that were restored in the parent restore map not
        //      just in the parent entry map. the old code did not, and I think
        //      this may have actually been a bug in collectEntries.
        //
        //    - also copy footprint entries from the live snapshot -- not just
        //      propagated/dirty stuff from the live snapshot, so it's a bit of
        //      a pre-load (though not a pre-hot-restore)

        for (auto const& txBundle : cluster)
        {
            auto const& footprint =
                txBundle.getTx()->sorobanResources().footprint;
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

    std::optional<LedgerEntry>
    getLiveEntryOpt(LedgerKey const& key) const
    {
        auto it0 = mThreadEntryMap.find(key);
        if (it0 != mThreadEntryMap.end())
        {
            return it0->second.mLedgerEntry;
        }
        auto rop = mThreadRestoredEntries.getEntryOpt(key);
        if (rop)
        {
            return rop;
        }
        auto res = mLiveSnapshot->load(key);
        return res ? std::make_optional(*res) : std::nullopt;
    }

    void
    commitThreadChangesToGlobal(AppConnector& app,
                                GlobalParallelApplyLedgerState& global,
                                ApplyStage const& stage)
    {
        releaseAssert(threadIsMain() ||
                      app.threadIsType(Application::ThreadType::APPLY));
        auto readWriteSet = getReadWriteKeysForStage(stage);
        writeDirtyThreadEntryMapEntriesToGlobalEntryMap(
            mThreadEntryMap, global.mGlobalEntryMap, readWriteSet);
        global.mGlobalRestoredEntries.addRestoresFrom(mThreadRestoredEntries);
    }
};

class OpParallelApplyLedgerState
{
    // Read-only access to the parent stage-spanning state.
    ThreadParallelApplyLedgerState const& mThreadState;

    // Contains keys restored during this op. As with the thread RestoredEntries
    // set, this is not just a key set but also contains entry values at the
    // point in time the entry was restored, and can be overridden by entries
    // in the mOpEntryMap.
    RestoredEntries mOpRestoredEntries;

    // Contains entries changed during this op. As with all such maps, if a
    // soroban entry is in this map, it must also have a TTL entry. Entries in
    // this map may also be set to nullopt, indicating that the entry was
    // _deleted_ in this op.
    //
    // Any entry in this map is implicitly dirty.
    OpModifiedEntryMap mOpEntryMap;

  public:
    OpParallelApplyLedgerState(ThreadParallelApplyLedgerState const& parent)
        : mThreadState(parent)
    {
    }

    void
    commitOpChangesToThread()
    {
    }

    // Any read should read _through_ these from bottom-to-top: first look in
    // the mOpEntryMap, then the mEntryMap, then the mLiveSnapshot, and finally
    // (if the read is a restoring-read) the mHotArchiveSnapshot.
    //
    // Any write should be directed to mOpEntryMap during the transaction
    // execution. At the end of each transaction, mOpEntryMap will be flushed
    // back to mEntryMap.
    //
    // The reason mEntryMap and mOpEntryMap are separate is that we sometimes
    // want to ask "was an entry created during this op?" and to do that we must
    // compare the two.

    // An additional complication is that every time you look up an entry, you
    // typically have to also look up its TTL, which is a separate entry also
    // stored in these maps.

    // Legal state transitions:
    //
    // each of these makes the entry dirty in the mParallelApplyEntryMap
    //
    //    evicted (not live, only in hot) => restored-live
    //    archived (TTL-expired-but-live) => restored-live
    //    live => deleted
    //    live => live modified
    //    nonexistent => created
    //    deleted => recreated
};

}