// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "ledger/ImmutableLedgerView.h"
#include "ledger/InMemorySorobanState.h"
#include "ledger/LedgerEntryScope.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTypeUtils.h"
#include "transactions/ParallelApplyStage.h"
#include "transactions/TransactionFrameBase.h"
#include "xdr/Stellar-ledger-entries.h"
#include <array>
#include <unordered_set>

namespace stellar
{

class InMemorySorobanState;
class GlobalParallelApplyLedgerState;

class ParallelLedgerInfo
{

  public:
    ParallelLedgerInfo(uint32_t version, uint32_t seq, uint32_t reserve,
                       TimePoint time, Hash const& id)
        : ledgerVersion(version)
        , ledgerSeq(seq)
        , baseReserve(reserve)
        , closeTime(time)
        , networkID(id)
    {
    }

    uint32_t
    getLedgerVersion() const
    {
        return ledgerVersion;
    }
    uint32_t
    getLedgerSeq() const
    {
        return ledgerSeq;
    }
    uint32_t
    getBaseReserve() const
    {
        return baseReserve;
    }
    TimePoint
    getCloseTime() const
    {
        return closeTime;
    }
    Hash
    getNetworkID() const
    {
        return networkID;
    }

  private:
    uint32_t ledgerVersion;
    uint32_t ledgerSeq;
    uint32_t baseReserve;
    TimePoint closeTime;
    Hash networkID;
};

// Whether soroban-typed (CONTRACT_DATA/CONTRACT_CODE/TTL) entry changes may
// bypass the LedgerTxn entirely: from PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION
// on, these entries persist via the level-0 shard buckets and the in-memory
// Soroban state, never via SQL; per-tx meta is built from TxEffects during
// apply; eviction gets its modified-key answers from an explicit key set; and
// the LedgerTxnRoot entry cache is cleared on every commit. The only remaining
// ltx consumer is the invariant subsystem, so the bypass is enabled exactly
// when no invariants are configured.
bool bypassLtxForSorobanEntries(Config const& cfg);

// The set of read-write footprint keys (plus their TTL keys) of all the
// transactions in a stage. Consulted by the thread-change merge to tell
// read-only TTL bumps (max-merged) apart from write conflicts. Depends only
// on the stage's (static) tx set, so it can be computed concurrently with
// the stage's execution.
ParallelApplyLedgerKeySet getReadWriteKeysForStage(ApplyStage const& stage);

class ThreadParallelApplyLedgerState
    : public LedgerEntryScope<StaticLedgerEntryScope::ThreadParApply>
{
    // Copy of the LCL state applyView from the global state, with fresh
    // file caches for thread safety.
    ApplyLedgerView mLCLApplyView;

    // Reference to the live in-memory Soroban state. For Soroban entries
    // (CONTRACT_DATA, CONTRACT_CODE, TTL), query this in-memory state instead
    // of mLiveSnapshot.
    InMemorySorobanState const& mInMemorySorobanState;

    // Reference to the Soroban configuration common for all the transactions
    // and thus common for all the threads.
    SorobanNetworkConfig const& mSorobanConfig;

    rust::Box<rust_bridge::SorobanModuleCache> mModuleCache;

    // Contains entries restored by any tx/op in the current thread's tx cluster
    // from the current stage of the parallel apply phase. Any entry should only
    // be in one of the two sub-maps here, live or hot. The entry in the map is
    // the entry value _at the time of restoration_ which may be overridden by
    // the entry map.
    RestoredEntries mThreadRestoredEntries;

    // Contains entries that were restored in previous stages of the parallel
    // execution phase. These are not considered as restorations that happened
    // in this thread for the sake of meta generation; but they _are_ consulted
    // when deciding if a given entry has already been restored (to inhibit
    // double-restores).
    RestoredEntries mPreviouslyRestoredEntries;

    // Contains all entries accessed by any tx/op in the current thread's
    // tx cluster from the current stage of the parallel apply phase. As with
    // the live entry map, any soroban entry in here must have an associated TTL
    // entry.
    ParallelApplyEntryMap<staticScope> mThreadEntryMap;

    // Contains a buffered set of RO TTL bumps that should only be observed
    // when/if the corresponding entry is modified, otherwise they are merged
    // (by taking maximums) into the global map at the end of the thread's life.
    ParallelApplyLedgerKeyMap<uint32_t> mRoTTLBumps;

    void collectClusterFootprintEntriesFromGlobal(
        AppConnector& app, GlobalParallelApplyLedgerState const& global,
        Cluster const& cluster);

    void upsertEntry(LedgerKey const& key,
                     ThreadParApplyLedgerEntry const& entry, uint32_t ledgerSeq,
                     bool isNew = false);
    void eraseEntry(LedgerKey const& key, bool isNew = false);
    void
    commitChangeFromSuccessfulTx(ParallelApplyLedgerKey const& key,
                                 ThreadParApplyLedgerEntryOpt const& entryOpt,
                                 ParallelApplyLedgerKeySet const& roTTLSet);

  public:
    ThreadParallelApplyLedgerState(AppConnector& app,
                                   GlobalParallelApplyLedgerState const& global,
                                   Cluster const& cluster, size_t clusterIdx);

    // For every soroban LE in `txBundle`s RW footprint, ensure we've flushed
    // any buffered RO TTL bumps stored in `mRoTTLBumps` to the
    // `mThreadEntryMap`.
    //
    // We do so because this tx that does an RW access to the LE:
    //
    //   - _Will_ be clustered with all other RO and RW txs touching the LE, so
    //   we
    //     don't need to worry about other clusters touching this LE or bumping
    //     its TTL in parallel. This LE and its TTL are sequentialized in this
    //     cluster.
    //
    //   - _Might_ be clustered with an earlier tx that did an RO TTL bump of
    //   the
    //     LE, which could have changed the cost of the LE write happening in
    //     this tx. We do have to worry about that!
    //
    // So: for correct accounting of the write happening in this tx, we have to
    // flush any pending RO TTL bumps that interfere with its RW footprint.
    void flushRoTTLBumpsInTxWriteFootprint(TxBundle const& txBundle);

    // Ensure that for each remaining RO TTL bump in `mRoTTLBumps`, the
    // TTL entry is present in the `mThreadEntryMap` and is >= the bump TTL.
    void flushRemainingRoTTLBumps();

    ParallelApplyEntryMap<staticScope> const& getEntryMap() const;
    ParallelApplyEntryMap<staticScope>& getEntryMap();

    // Extract this thread's dirty CONTRACT_DATA/CONTRACT_CODE changes as
    // (unsorted) BucketEntries for an optimistic level-0 shard write. TTL
    // entries are excluded (they need cross-thread reconciliation in the
    // global state); entries created and deleted within this ledger are
    // skipped entirely, matching the ltx's annihilation of
    // created-then-erased entries. Cluster RW footprints are disjoint
    // within a stage, and nothing after the parallel phase modifies these
    // entry types, so the extracted entries are final for this ledger
    // (modulo later stages, which shadow them by shard order).
    std::vector<BucketEntry> extractDirtySorobanShardEntries() const;

    RestoredEntries const& getRestoredEntries() const;

    OptionalEntryT getLiveEntryOpt(LedgerKey const& key) const;
    bool entryWasRestored(LedgerKey const& key) const;

    void setEffectsDeltaFromSuccessfulTx(ParallelTxSuccessVal const& res,
                                         ParallelLedgerInfo const& ledgerInfo,
                                         TxEffects& effects) const;

    void commitChangesFromSuccessfulTx(ParallelTxSuccessVal const& res,
                                       TxBundle const& txBundle);

    // The applyView ledger sequence number is one less than the
    // applying ledger sequence number.
    uint32_t getSnapshotLedgerSeq() const;

    SorobanNetworkConfig const& getSorobanConfig() const;

    ApplyLedgerView const& getSnapshot() const;

    rust::Box<rust_bridge::SorobanModuleCache> const& getModuleCache() const;
};

class GlobalParallelApplyLedgerState
    : public LedgerEntryScope<StaticLedgerEntryScope::GlobalParApply>
{
    // Contains the full LCL state applyView from the start of the ledger
    // close, providing access to both the live bucket list and the hot archive
    // bucket list. Note that this does not reflect changes from the classic
    // apply phase, but is a applyView of the start of the ledger.
    ApplyLedgerView mLCLApplyView;

    // Contains an exact one-to-one in-memory mapping of the live applyView for
    // CONTRACT_DATA, CONTRACT_CODE, and TTL entries. For these entry types,
    // only mInMemorySorobanState should be queried. If the in-memory state
    // returns null for a key, it does NOT indicate a "cache miss," rather the
    // key does not exist as part of the live state.
    InMemorySorobanState const& mInMemorySorobanState;

    // Reference to the common global Soroban configuration to use during the
    // transaction application.
    SorobanNetworkConfig const& mSorobanConfig;

    // Contains restorations that happened during each stage of the parallel
    // soroban phase. As with mGlobalEntryMap, this is propagated stage to
    // stage by being split into per-thread maps and re-merged at the end of
    // the stage, before begin committed to the ltx at the end of the phase.
    // As with restorations inside the thread, these entries are the
    // restored values _at their time of restoration_ which may be further
    // overridden by mGlobalEntryMap.
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
    //
    // The map is split into shards by key hash so the post-stage merge of
    // the per-thread maps can run on parallel workers: each worker owns one
    // shard and scans all thread maps for that shard's keys, so the shards
    // never need locking.
    static constexpr size_t kGlobalEntryMapShards = 16;
    std::array<ParallelApplyEntryMap<staticScope>, kGlobalEntryMapShards>
        mGlobalEntryMapShards;

    static size_t
    globalMapShardOf(ParallelApplyLedgerKey const& key)
    {
        // Mix the (cached) key hash and take the top bits so the shard index
        // stays uncorrelated with the in-shard bucket index.
        return (key.hash() * 0x9E3779B97F4A7C15ull) >> 60;
    }

    ParallelApplyEntryMap<staticScope>&
    globalMapShardFor(ParallelApplyLedgerKey const& key)
    {
        return mGlobalEntryMapShards[globalMapShardOf(key)];
    }

    void preParallelApplyAndCollectModifiedClassicEntries(
        AppConnector& app, AbstractLedgerTxn& ltx,
        std::vector<ApplyStage> const& stages);

    void
    readOnlyPreParallelApply(AppConnector& app,
                             std::vector<TxBundle const*> const& txBundles,
                             PreApplyAccountOverlay const& overlay);

    void commitBufferedPreParallelApplyWrites(
        AppConnector& app, AbstractLedgerTxn& ltx,
        std::vector<TxBundle const*> const& txBundles);

    void collectModifiedClassicEntries(AbstractLedgerTxn& ltx,
                                       std::vector<ApplyStage> const& stages);

    bool maybeMergeRoTTLBumps(ParallelApplyLedgerKey const& key,
                              GlobalParallelApplyEntry const& newEntry,
                              GlobalParallelApplyEntry& oldEntry,
                              ParallelApplyLedgerKeySet const& readWriteSet);

    void commitChangeFromThread(ThreadParallelApplyLedgerState const& thread,
                                ParallelApplyLedgerKey const& key,
                                ThreadParallelApplyEntry&& parEntry,
                                ParallelApplyLedgerKeySet const& readWriteSet,
                                bool skipSorobanDataAndCode);

    // Merge the entries of all threads that route to the given shard. Used
    // as the per-worker body of the parallelized commitChangesFromThreads:
    // distinct shards touch disjoint entries (each entry is moved out of its
    // thread map by exactly one shard worker), so concurrent calls for
    // different shards need no synchronization.
    void commitShardChangesFromThreads(
        size_t shardIdx,
        std::vector<std::unique_ptr<ThreadParallelApplyLedgerState>> const&
            threads,
        ParallelApplyLedgerKeySet const& readWriteSet,
        bool skipSorobanDataAndCode);

  public:
    GlobalParallelApplyLedgerState(AppConnector& app, ApplyLedgerView applyView,
                                   AbstractLedgerTxn& ltx,
                                   std::vector<ApplyStage> const& stages,
                                   InMemorySorobanState const& inMemoryState,
                                   SorobanNetworkConfig const& sorobanConfig);

    // Look up an entry in the (sharded) global entry map; returns nullptr if
    // absent.
    GlobalParallelApplyEntry const*
    findInGlobalEntryMap(ParallelApplyLedgerKey const& key) const;
    RestoredEntries const& getRestoredEntries() const;

    // skipSorobanDataAndCode: used for the last stage when soroban entries
    // bypass the ltx -- CONTRACT_DATA/CODE entries are final at thread
    // completion (consumed directly by the shard writers and the in-memory
    // state updater), so they need not be merged into the global map. TTL
    // entries are always merged (cross-thread reconciliation), as are
    // restored-entry records and anything classic.
    // readWriteSet: the stage's read-write key set (see
    // getReadWriteKeysForStage), typically precomputed on the apply pool
    // while the stage's clusters run.
    void commitChangesFromThreads(
        AppConnector& app,
        std::vector<std::unique_ptr<ThreadParallelApplyLedgerState>> const&
            threads,
        ParallelApplyLedgerKeySet const& readWriteSet,
        bool skipSorobanDataAndCode);

    // Extract the dirty TTL entries (reconciled across all threads and
    // stages) as (unsorted) BucketEntries for a level-0 shard write. Must
    // be called after the last stage's commitChangesFromThreads (so
    // read-only TTL bumps have been max-merged) and before
    // commitChangesToLedgerTxn (which moves entries out).
    std::vector<BucketEntry> extractDirtyTTLShardEntries() const;

    // Consumes the global entry map: moves entries into the LedgerTxn
    // instead of copying. Must only be called once, as the final operation
    // on this state (entries are left in a moved-from state afterwards).
    // skipSorobanEntries: when soroban entries bypass the ltx (see
    // bypassLtxForSorobanEntries), CONTRACT_DATA/CODE/TTL entries are not
    // written to the ltx at all; restored-entry markers are still recorded.
    void commitChangesToLedgerTxn(AbstractLedgerTxn& ltx,
                                  bool skipSorobanEntries);

    // The applyView ledger sequence number is one less than the
    // applying ledger sequence number.
    uint32_t getSnapshotLedgerSeq() const;

#ifdef BUILD_TESTS
    // Sub-timings (ms) of the global setup phase, accumulated during
    // construction and read out by LedgerManagerImpl into its phase-timing
    // record:
    //   - seq-dependency check + sequential pre-apply loop
    //   - read-only pre-apply (itself parallelized across workers)
    //   - commit of buffered pre-apply writes
    //   - collection of modified classic entries
    //   - pre-load of read-only Soroban entries (+ TTLs)
    double mSetupSeqCheckMs = 0;
    double mSetupReadOnlyMs = 0;
    double mSetupCommitWritesMs = 0;
    double mSetupCollectClassicMs = 0;
    double mSetupPreloadSorobanRoMs = 0;
    // Sub-timings of mSetupSeqCheckMs (the sequential preParallelApply loop):
    // commonValid, signature processing, operation checkValid, and the
    // buffered-write commit.
    double mSetupSeqCommonValidMs = 0;
    double mSetupSeqProcessSigsMs = 0;
    double mSetupSeqCheckValidMs = 0;
    double mSetupSeqWriteMs = 0;
#endif

    // Constructor requires access to mInMemorySorobanState
    friend ThreadParallelApplyLedgerState::ThreadParallelApplyLedgerState(
        AppConnector& app, GlobalParallelApplyLedgerState const& global,
        Cluster const& cluster, size_t clusterIdx);
};

class TxParallelApplyLedgerState
    : public LedgerEntryScope<StaticLedgerEntryScope::TxParApply>
{
    // Read-only access to the parent stage-spanning state.
    ThreadParallelApplyLedgerState const& mThreadState;

    // Guard that _deactivates reading_ ScopedLedgerEntries from the
    // mThreadState while this tx state is alive, to prevent accidental
    // access to stale data. Any access must scopeAdoptEntryFrom the thread
    // state first.
    DeactivateScopeGuard<StaticLedgerEntryScope::ThreadParApply>
        mThreadStateDeactivateGuard;

    // Contains keys restored during this tx. As with the thread RestoredEntries
    // set, this is not just a key set but also contains entry values at the
    // point in time the entry was restored, and can be overridden by entries
    // in the mTxEntryMap.
    RestoredEntries mTxRestoredEntries;

    // Contains entries changed during this tx. As with all such maps, if a
    // soroban entry is in this map, it must also have a TTL entry. Entries in
    // this map may also be set to nullopt, indicating that the entry was
    // _deleted_ in this tx.
    //
    // Deletions will only be added if there was a previously-existing entry to
    // delete in the parent (thread or live applyView) maps. A stray delete here
    // (eg. a std::nullptr entry) not-related to a previous live LE is a bug.
    //
    // Any entry in this map is implicitly dirty. Merely loading data from the
    // thread map or the live applyView does not add an entry to this map.
    TxModifiedEntryMap mTxEntryMap;

  public:
    TxParallelApplyLedgerState(ThreadParallelApplyLedgerState const& parent);
    OptionalEntryT getLiveEntryOpt(LedgerKey const& key) const;

    // Upsert the entry and sets the lastModifiedLedgerSeq to the given ledger
    // sequence number.
    void upsertEntry(LedgerKey const& key, LedgerEntry const& entry,
                     uint32_t ledgerSeq);
    bool eraseEntryIfExists(LedgerKey const& key);
    bool entryWasRestored(LedgerKey const& key) const;
    void addHotArchiveRestore(LedgerKey const& key, LedgerEntry const& entry,
                              LedgerKey const& ttlKey,
                              LedgerEntry const& ttlEntry);
    void addLiveBucketlistRestore(LedgerKey const& key,
                                  LedgerEntry const& entry,
                                  LedgerKey const& ttlKey,
                                  LedgerEntry const& ttlEntry);
    std::optional<ParallelTxSuccessVal> takeResult(bool success);
    uint32_t getSnapshotLedgerSeq() const;
};

class LedgerAccessHelper
{
  public:
    // getLedgerEntry returns an entry if it exists, else nullopt.
    virtual std::optional<LedgerEntry>
    getLedgerEntryOpt(LedgerKey const& key) = 0;

    virtual void upsertLedgerEntry(LedgerKey const& key,
                                   LedgerEntry const& entry) = 0;

    // erase returns true if the entry was erased, false if it wasn't present.
    // as with upsert, this is interpreted narrowly to mean that an erase
    // (essentially a nullptr / std::nullopt upsert) is only performed if there
    // was no populated/non-null entry in any parent level of the ledger state.
    virtual bool eraseLedgerEntryIfExists(LedgerKey const& key) = 0;

    virtual uint32_t getLedgerVersion() = 0;
    virtual uint32_t getLedgerSeq() = 0;
};

class PreV23LedgerAccessHelper : virtual public LedgerAccessHelper
{
  protected:
    PreV23LedgerAccessHelper(AbstractLedgerTxn& ltx);

    AbstractLedgerTxn& mLtx;

    std::optional<LedgerEntry> getLedgerEntryOpt(LedgerKey const& key) override;
    void upsertLedgerEntry(LedgerKey const& key,
                           LedgerEntry const& entry) override;
    bool eraseLedgerEntryIfExists(LedgerKey const& key) override;
    uint32_t getLedgerVersion() override;
    uint32_t getLedgerSeq() override;
};

class ParallelLedgerAccessHelper : virtual public LedgerAccessHelper
{

  protected:
    ParallelLedgerAccessHelper(
        ThreadParallelApplyLedgerState const& threadState,
        ParallelLedgerInfo const& ledgerInfo);

    ParallelLedgerInfo const& mLedgerInfo;
    TxParallelApplyLedgerState mTxState;

    std::optional<LedgerEntry> getLedgerEntryOpt(LedgerKey const& key) override;
    void upsertLedgerEntry(LedgerKey const& key,
                           LedgerEntry const& entry) override;
    bool eraseLedgerEntryIfExists(LedgerKey const& key) override;
    uint32_t getLedgerVersion() override;
    uint32_t getLedgerSeq() override;
};
}
