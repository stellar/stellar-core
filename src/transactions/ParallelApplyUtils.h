#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/InMemorySorobanState.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTypeUtils.h"
#include "transactions/ParallelApplyStage.h"
#include "transactions/TransactionFrameBase.h"
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

class ThreadParallelApplyLedgerState
{
    // Copies of snapshots from the global state.
    SearchableHotArchiveSnapshotConstPtr mHotArchiveSnapshot;
    SearchableSnapshotConstPtr mLiveSnapshot;

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
    ParallelApplyEntryMap mThreadEntryMap;

    // Contains a buffered set of RO TTL bumps that should only be observed
    // when/if the corresponding entry is modified, otherwise they are merged
    // (by taking maximums) into the global map at the end of the thread's life.
    UnorderedMap<LedgerKey, uint32_t> mRoTTLBumps;

    void collectClusterFootprintEntriesFromGlobal(
        AppConnector& app, GlobalParallelApplyLedgerState const& global,
        Cluster const& cluster);

    void upsertEntry(LedgerKey const& key, LedgerEntry const& entry,
                     uint32_t ledgerSeq);
    void eraseEntry(LedgerKey const& key);
    void
    commitChangeFromSuccessfulOp(LedgerKey const& key,
                                 std::optional<LedgerEntry> const& entryOpt,
                                 UnorderedSet<LedgerKey> const& roTTLSet);

  public:
    ThreadParallelApplyLedgerState(AppConnector& app,
                                   GlobalParallelApplyLedgerState const& global,
                                   Cluster const& cluster);

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
    void flushRoTTLBumpsInTxWriteFootprint(const TxBundle& txBundle);

    // Ensure that for each remaining RO TTL bump in `mRoTTLBumps`, the
    // TTL entry is present in the `mThreadEntryMap` and is >= the bump TTL.
    void flushRemainingRoTTLBumps();

    ParallelApplyEntryMap const& getEntryMap() const;

    RestoredEntries const& getRestoredEntries() const;

    std::optional<LedgerEntry> getLiveEntryOpt(LedgerKey const& key) const;
    bool entryWasRestored(LedgerKey const& key) const;

    void setEffectsDeltaFromSuccessfulOp(ParallelTxReturnVal const& res,
                                         ParallelLedgerInfo const& ledgerInfo,
                                         TxEffects& effects) const;

    void commitChangesFromSuccessfulOp(ParallelTxReturnVal const& res,
                                       TxBundle const& txBundle);

    // The snapshot ledger sequence number is one less than the
    // applying ledger sequence number.
    uint32_t getSnapshotLedgerSeq() const;

    SorobanNetworkConfig const& getSorobanConfig() const;

    SearchableHotArchiveSnapshotConstPtr const& getHotArchiveSnapshot() const;

    rust::Box<rust_bridge::SorobanModuleCache> const& getModuleCache() const;
};

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
    // Note that only classic entries should be queried from mLiveSnapshot. For
    // Soroban entries query mInMemorySorobanState instead.
    SearchableSnapshotConstPtr mLiveSnapshot;

    // Contains an exact one-to-one in-memory mapping of the live snapshot for
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
    ParallelApplyEntryMap mGlobalEntryMap;

    void
    commitChangeFromThread(LedgerKey const& key,
                           ParallelApplyEntry const& parEntry,
                           std::unordered_set<LedgerKey> const& readWriteSet);

    void commitChangesFromThread(AppConnector& app,
                                 ThreadParallelApplyLedgerState const& thread,
                                 ApplyStage const& stage);

  public:
    GlobalParallelApplyLedgerState(AppConnector& app, AbstractLedgerTxn& ltx,
                                   std::vector<ApplyStage> const& stages,
                                   InMemorySorobanState const& inMemoryState,
                                   SorobanNetworkConfig const& sorobanConfig);

    ParallelApplyEntryMap const& getGlobalEntryMap() const;
    RestoredEntries const& getRestoredEntries() const;

    void commitChangesFromThreads(
        AppConnector& app,
        std::vector<std::unique_ptr<ThreadParallelApplyLedgerState>> const&
            threads,
        ApplyStage const& stage);

    void commitChangesToLedgerTxn(AbstractLedgerTxn& ltx) const;

    // The snapshot ledger sequence number is one less than the
    // applying ledger sequence number.
    uint32_t getSnapshotLedgerSeq() const;

    // Constructor requires access to mInMemorySorobanState
    friend ThreadParallelApplyLedgerState::ThreadParallelApplyLedgerState(
        AppConnector& app, GlobalParallelApplyLedgerState const& global,
        Cluster const& cluster);
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
    // Deletions will only be added if there was a previously-existing entry to
    // delete in the parent (thread or live snapshot) maps. A stray delete here
    // (eg. a std::nullptr entry) not-related to a previous live LE is a bug.
    //
    // Any entry in this map is implicitly dirty. Merely loading data from the
    // thread map or the live snapshot does not add an entry to this map.
    OpModifiedEntryMap mOpEntryMap;

  public:
    OpParallelApplyLedgerState(ThreadParallelApplyLedgerState const& parent);
    std::optional<LedgerEntry> getLiveEntryOpt(LedgerKey const& key) const;

    // Upsert the entry and sets the lastModifiedLedgerSeq to the given ledger
    // sequence number.
    bool upsertEntry(LedgerKey const& key, LedgerEntry const& entry,
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
    ParallelTxReturnVal takeSuccess();
    ParallelTxReturnVal takeFailure();
    uint32_t getSnapshotLedgerSeq() const;
};

class LedgerAccessHelper
{
  public:
    // getLedgerEntry returns an entry if it exists, else nullopt.
    virtual std::optional<LedgerEntry>
    getLedgerEntryOpt(LedgerKey const& key) = 0;

    // upsert returns true if the entry was created, false if it was updated.
    // "created" here is interpreted narrowly to mean there was no
    // populated/non-null entry in any parent level of the ledger state; a
    // "local" map-insert that shadows an existing entry is not considered a
    // create.
    virtual bool upsertLedgerEntry(LedgerKey const& key,
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
    bool upsertLedgerEntry(LedgerKey const& key,
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
    OpParallelApplyLedgerState mOpState;

    std::optional<LedgerEntry> getLedgerEntryOpt(LedgerKey const& key) override;
    bool upsertLedgerEntry(LedgerKey const& key,
                           LedgerEntry const& entry) override;
    bool eraseLedgerEntryIfExists(LedgerKey const& key) override;
    uint32_t getLedgerVersion() override;
    uint32_t getLedgerSeq() override;
};
}
