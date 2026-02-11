---
name: subsystem-summary-of-ledger
description: "read this skill for a token-efficient summary of the ledger subsystem"
---

# Ledger Subsystem — Technical Summary

## Overview

The ledger subsystem is the core of stellar-core's state management. It orchestrates ledger closing (applying transaction sets to produce new ledger states), manages the "last closed ledger" (LCL) state, provides a transactional in-memory layer (`LedgerTxn`) for reading/writing ledger entries during transaction processing, maintains in-memory Soroban contract state, manages Soroban network configuration, and produces ledger close metadata for downstream consumers (e.g., Horizon).

The subsystem has a two-thread architecture: a **main thread** handles consensus and publishes state, while a **apply thread** executes transactions and commits state. The invariant `LCL <= A <= Q <= H` tracks ledger progress across these threads (H=heard, Q=queued, A=applying, LCL=last closed).

## Key Files

- **LedgerManager.h / LedgerManagerImpl.h / LedgerManagerImpl.cpp** — Abstract interface and implementation for ledger lifecycle: applying, closing, state management, parallel apply orchestration.
- **LedgerTxn.h / LedgerTxnImpl.h / LedgerTxn.cpp** — Nestable in-memory transactional layer over the database for ledger entries; core data mutation path.
- **LedgerTxnEntry.h / .cpp** — Handle types (`LedgerTxnEntry`, `ConstLedgerTxnEntry`) for accessing active entries in a `LedgerTxn`.
- **LedgerTxnHeader.h / .cpp** — Handle type for accessing the `LedgerHeader` within a `LedgerTxn`.
- **LedgerTxnOfferSQL.cpp** — SQL-specific bulk operations for offer entries (upsert/delete) used by `LedgerTxnRoot`.
- **InternalLedgerEntry.h / .cpp** — Extended ledger entry types (`InternalLedgerEntry`, `InternalLedgerKey`) that wrap XDR `LedgerEntry`/`LedgerKey` with additional internal-only types (sponsorship tracking, max-seq-num-to-apply).
- **NetworkConfig.h / .cpp** — `SorobanNetworkConfig`: reads/writes all Soroban-related configuration from ledger entries, provides fee and resource limit accessors.
- **InMemorySorobanState.h / .cpp** — `InMemorySorobanState`: in-memory cache of all live Soroban contract data, code, and TTL entries for fast lookups during transaction execution.
- **LedgerStateSnapshot.h / .cpp** — Unified read-only snapshot interfaces (`LedgerSnapshot`, `CompleteConstLedgerState`) abstracting over SQL and BucketList backends.
- **LedgerCloseMetaFrame.h / .cpp** — Wrapper around `LedgerCloseMeta` XDR for building per-ledger metadata during close.
- **LedgerEntryScope.h / .cpp** — Compile-time and runtime scope checking for `LedgerEntry` usage across threads and ledger phases (`ScopedLedgerEntry<S>`, `LedgerEntryScope<S>`).
- **SharedModuleCacheCompiler.h / .cpp** — Multi-threaded Wasm contract compilation for Soroban module cache.
- **SorobanMetrics.h / .cpp** — Aggregated metrics for Soroban transaction execution (CPU, memory, IO, fees).
- **LedgerHeaderUtils.h / .cpp** — Utilities for storing/loading `LedgerHeader` to/from the database.
- **LedgerTypeUtils.h / .cpp** — Helpers for TTL key derivation, liveness checks, and Soroban entry classification.
- **LedgerHashUtils.h** — Hash functions for `LedgerKey`, `Asset`, `InternalLedgerKey` used in unordered containers.
- **TrustLineWrapper.h / .cpp** — Safe wrapper for trust line operations (balance, liabilities, authorization) with special issuer handling.
- **CheckpointRange.h / .cpp** — Represents half-open ranges of history checkpoints.
- **LedgerRange.h / .cpp** — Represents half-open ranges of ledger sequence numbers.
- **FlushAndRotateMetaDebugWork.h / .cpp** — Background work item for flushing and rotating debug meta streams.
- **P23HotArchiveBug.h / .cpp** — Protocol 23 hot archive corruption verification and fix data.

---

## Key Classes and Data Structures

### `LedgerManager` (abstract interface)

Defines the public API for ledger lifecycle management. States: `LM_BOOTING_STATE`, `LM_SYNCED_STATE`, `LM_CATCHING_UP_STATE`.

**Key methods:**
- `valueExternalized(LedgerCloseData)` — Called by Herder when SCP reaches consensus; triggers ledger apply.
- `applyLedger(LedgerCloseData, calledViaExternalize)` — Core ledger application: processes fees/seqnums, applies transactions, applies upgrades, seals to buckets/DB.
- `advanceLedgerStateAndPublish(...)` — Post-apply: publishes to history, updates LCL, triggers next ledger.
- `getLastClosedLedgerHeader()` / `getLastClosedLedgerNum()` — Access LCL state.
- `getLastClosedSnapshot()` — Returns immutable BucketList snapshot of LCL.
- `getLastClosedSorobanNetworkConfig()` — Returns Soroban config as of LCL (main thread only).
- `startNewLedger()` / `loadLastKnownLedger()` — Startup paths.
- `setLastClosedLedger(...)` — Used by catchup after bucket-apply to reset LCL.
- `getSorobanMetrics()` / `getModuleCache()` — Access apply-thread state.
- `markApplyStateReset()` — Signals that apply state must be re-initialized (e.g., after catchup).

**Genesis constants:** `GENESIS_LEDGER_SEQ=1`, `GENESIS_LEDGER_VERSION=0`, `GENESIS_LEDGER_BASE_FEE=100`, `GENESIS_LEDGER_BASE_RESERVE=100000000`, `GENESIS_LEDGER_TOTAL_COINS=1000000000000000000`.

### `LedgerManagerImpl`

Concrete implementation. Owns `ApplyState` and cached LCL state.

**Key members:**
- `mApplyState` (`ApplyState`) — Encapsulates all state for the apply thread (metrics, module cache, in-memory Soroban state). Has phase machine: `SETTING_UP_STATE → READY_TO_APPLY → APPLYING → COMMITTING → READY_TO_APPLY`.
- `mLastClosedLedgerState` (`CompleteConstLedgerStatePtr`) — Complete snapshot of LCL (bucket snapshot, hot archive snapshot, soroban config, header, HAS).
- `mLastClose` (`VirtualClock::time_point`) — Timestamp of last close.
- `mLedgerStateMutex` — Guards ledger state during apply.
- `mCurrentlyApplyingLedger` — Indicates background apply thread is active.
- `mNextMetaToEmit` — Buffered meta frame awaiting emission.
- `mMetaStream` / `mMetaDebugStream` — XDR output streams for ledger close meta.
- `mState` — Current `LedgerManager::State`.

**Key internal methods:**
- `processFeesSeqNums(...)` — Charges fees and increments sequence numbers before tx apply.
- `applyTransactions(...)` — Dispatches to `applySequentialPhase` (classic) and `applyParallelPhase` (Soroban).
- `applyParallelPhase(...)` — Orchestrates parallel Soroban execution via stages/clusters.
- `applySorobanStages(...)` / `applySorobanStageClustersInParallel(...)` — Spawns worker threads for Soroban tx execution.
- `applyThread(...)` — Entry point for each Soroban worker thread.
- `sealLedgerTxnAndStoreInBucketsAndDB(...)` — Seals the LedgerTxn, writes to BucketList and DB, runs invariants.
- `storePersistentStateAndLedgerHeaderInDB(...)` — Persists ledger header and HAS.
- `advanceBucketListSnapshotAndMakeLedgerState(...)` — Updates BucketList snapshot, constructs `CompleteConstLedgerState`.
- `advanceLastClosedLedgerState(...)` — Updates cached LCL variables.
- `prefetchTransactionData(...)` / `prefetchTxSourceIds(...)` — Pre-loads entries into cache before apply.
- `ledgerCloseComplete(...)` — Called after `advanceLedgerStateAndPublish` completes all post-close work.

### `ApplyState` (inner class of `LedgerManagerImpl`)

Encapsulates state used by the primary apply thread. Only the primary apply thread may mutate it. Soroban execution threads see it as immutable.

**Key members:**
- `mMetrics` (`LedgerApplyMetrics`) — All apply-related metrics.
- `mModuleCache` (`rust::Box<SorobanModuleCache>`) — Reusable compiled Soroban module cache.
- `mCompiler` (`SharedModuleCacheCompiler`) — Background contract compiler, non-null only during compilation.
- `mInMemorySorobanState` (`InMemorySorobanState`) — Live Soroban state cache.
- `mPhase` — Current phase enum.

**Phase transitions:** `SETTING_UP_STATE → READY_TO_APPLY ↔ APPLYING → COMMITTING → READY_TO_APPLY`. Also `READY_TO_APPLY → SETTING_UP_STATE` when catchup resets state.

---

### `AbstractLedgerTxnParent` (abstract)

Base class for anything that can be the parent of a `LedgerTxn`. Provides interface for committing children, querying offers, loading entries, prefetching.

### `AbstractLedgerTxn` (extends `AbstractLedgerTxnParent`)

Adds transaction semantics: `commit()`, `rollback()`, `loadHeader()`, `create()`, `load()`, `erase()`, `loadWithoutRecord()`, `restoreFromLiveBucketList()`, `markRestoredFromHotArchive()`, bulk-load methods (`createWithoutLoading`, `updateWithoutLoading`, `eraseWithoutLoading`), and sealed-state extraction (`getChanges()`, `getDelta()`, `getAllEntries()`).

### `LedgerTxn` (concrete, extends `AbstractLedgerTxn`)

Nestable in-memory transaction. Can be a child of either another `LedgerTxn` or `LedgerTxnRoot`. Thread-affine: must be used from the same thread throughout its lifetime.

**Key inner class `LedgerTxn::Impl`:**
- `mEntry` (`EntryMap = UnorderedMap<InternalLedgerKey, LedgerEntryPtr>`) — Map of all entries modified/created/deleted in this transaction.
- `mActive` (`UnorderedMap<InternalLedgerKey, shared_ptr<EntryImplBase>>`) — Tracks which entries are currently "active" (safe to access). Entries are deactivated when a child is opened.
- `mMultiOrderBook` — In-memory order book for offers, grouped by asset pair, sorted by best-offer relation. Only consulted when `mActive` is empty.
- `mWorstBestOffer` (`WorstBestOfferMap`) — Cache to accelerate repeated `loadBestOffer` calls in nested LedgerTxns (critical for offer crossing loops).
- `mHeader` — Copy of `LedgerHeader` for this transaction level.
- `mRestoredEntries` (`RestoredEntries`) — Tracks entries restored from hot archive or live bucket list.
- `mIsSealed` — Once sealed, no more mutations; ready for extraction.
- `mConsistency` — `EXACT` or `EXTRA_DELETES` (relaxed for `eraseWithoutLoading`).

**Commit flow:** On `commit()`, all entries in `mEntry` are merged into the parent. If the parent is another `LedgerTxn`, entries go into the parent's `mEntry` map. If the parent is `LedgerTxnRoot`, entries are written to the database via SQL.

### `LedgerTxnRoot` (concrete, extends `AbstractLedgerTxnParent`)

The root of the `LedgerTxn` hierarchy. Connects to the database. One per application.

**Key inner class `LedgerTxnRoot::Impl`:**
- `mEntryCache` (`RandomEvictionCache<LedgerKey, CacheEntry>`) — LRU cache of recently-loaded entries from DB.
- `mBestOffers` — Cache of best offers per asset pair, loaded lazily from DB in batches.
- `mSearchableBucketListSnapshot` — BucketList snapshot for Soroban entry lookups.
- `mInMemorySorobanState` — Reference to the in-memory Soroban state for fast lookups of contract data/code/TTL.
- `mSession` — Database session wrapper.
- `mBulkLoadBatchSize` — Controls batch size for SQL bulk loads.
- `mTransaction` — Active SQL transaction (SERIALIZABLE isolation).
- Prefetch tracking: `mPrefetchHits`, `mPrefetchMisses`.

### `LedgerEntryPtr`

A smart pointer wrapper around `InternalLedgerEntry` with state tracking: `INIT` (created at this level), `LIVE` (modified at this level), `DELETED` (deleted at this level). Used within `LedgerTxn::Impl::mEntry`.

### `LedgerTxnEntry` / `ConstLedgerTxnEntry`

Client-facing handles for accessing ledger entries. Uses double-indirection (weak_ptr to Impl) to enforce the invariant that only the innermost LedgerTxn's entries are accessible. Move-only (no copy). Key methods: `current()`, `currentGeneralized()`, `deactivate()`, `erase()`.

### `LedgerTxnHeader`

Handle for accessing the `LedgerHeader` within a `LedgerTxn`. Same double-indirection pattern as `LedgerTxnEntry`.

---

### `InternalLedgerEntry` / `InternalLedgerKey`

Extended entry/key types wrapping XDR `LedgerEntry`/`LedgerKey` with additional discriminated-union variants:
- `LEDGER_ENTRY` — Normal XDR ledger entry.
- `SPONSORSHIP` — Tracks sponsoredID↔sponsoringID relationships.
- `SPONSORSHIP_COUNTER` — Counts how many entries an account sponsors.
- `MAX_SEQ_NUM_TO_APPLY` — Tracks maximum sequence number constraints.

These are used internally by the `LedgerTxn` subsystem and are not persisted to the database directly as their own table types—they are tracked alongside the regular ledger entries in the in-memory transaction maps.

---

### `SorobanNetworkConfig`

Holds all Soroban contract-related network configuration parameters loaded from ledger entries (config settings). Constructed via `loadFromLedger()`.

**Key setting groups:**
- **Contract size:** `maxContractSizeBytes`, `maxContractDataKeySizeBytes`, `maxContractDataEntrySizeBytes`.
- **Compute:** `ledgerMaxInstructions`, `txMaxInstructions`, `txMemoryLimit`, `feeRatePerInstructionsIncrement`.
- **Ledger access:** `ledgerMaxDiskReadEntries/Bytes`, `ledgerMaxWriteLedgerEntries/Bytes`, `txMaxDiskReadEntries/Bytes`, `txMaxWriteLedgerEntries/Bytes`, per-entry and per-KB fees.
- **Bandwidth:** `ledgerMaxTransactionSizesBytes`, `txMaxSizeBytes`, `feeTransactionSize1KB`.
- **Events:** `txMaxContractEventsSizeBytes`, `feeContractEventsSize1KB`.
- **State archival:** `StateArchivalSettings`, `EvictionIterator`, rent rates, lifetime bounds.
- **Parallel execution:** `ledgerMaxDependentTxClusters`.
- **SCP timing:** `ledgerTargetCloseTimeMilliseconds`, nomination/ballot timeout settings.
- **Cost model:** `cpuCostParams`, `memCostParams` (contract cost parameters for the Soroban host).

Static methods like `createLedgerEntriesForV20()`, `createCostTypesForV21/V22/V25()`, `createAndUpdateLedgerEntriesForV23()` handle protocol upgrade initialization of config entries.

---

### `InMemorySorobanState`

In-memory cache of all live Soroban contract data, contract code, and their TTLs. Updated each ledger. NOT thread-safe for concurrent reads+writes; callers must ensure exclusivity.

**Key members:**
- `mContractDataEntries` (`unordered_set<InternalContractDataMapEntry>`) — ContractData entries indexed by TTL key hash, using a custom polymorphic entry type to save memory (avoids storing the key twice).
- `mContractCodeEntries` (`unordered_map<uint256, ContractCodeMapEntryT>`) — ContractCode entries indexed by TTL key hash.
- `mPendingTTLs` — Temporary buffer for TTLs arriving before their data during initialization.
- `mContractCodeStateSize`, `mContractDataStateSize` — Running size totals for rent fee computation.

**Key methods:**
- `initializeStateFromSnapshot(snap, ledgerVersion)` — Populates from BucketList snapshot.
- `updateState(initEntries, liveEntries, deadEntries, header, sorobanConfig)` — Incremental per-ledger update.
- `get(LedgerKey)` — Returns entry or nullptr.
- `getSize()` — Total in-memory state size for rent calculations.
- `recomputeContractCodeSize(...)` — Recomputes code sizes after config/protocol upgrades.

---

### `LedgerSnapshot` / `CompleteConstLedgerState`

**`LedgerSnapshot`:** Short-lived read-only snapshot over either SQL (via `LedgerTxnReadOnly`) or BucketList (via `BucketSnapshotState`). Should not persist across ledger boundaries. Provides `getLedgerHeader()`, `getAccount()`, `load()`.

**`CompleteConstLedgerState`:** Immutable bundle of all ledger state at a specific sequence: BucketList snapshot, hot archive snapshot, Soroban config, ledger header, and HAS. Stored as the LCL in `LedgerManagerImpl::mLastClosedLedgerState`.

### `LedgerEntryWrapper` / `LedgerHeaderWrapper`

Variant wrappers that unify `LedgerTxnEntry`/`ConstLedgerTxnEntry`/`shared_ptr<LedgerEntry const>` (and `LedgerTxnHeader`/`shared_ptr<LedgerHeader>`) behind a common read interface, used by the snapshot abstractions.

---

### `LedgerCloseMetaFrame`

Wraps `LedgerCloseMeta` XDR. Built during `applyLedger()` to record per-transaction fee processing, tx execution meta, upgrade processing, evicted entries, and network configuration. Streamed to external consumers.

### `SharedModuleCacheCompiler`

Multi-threaded producer-consumer for compiling Soroban Wasm contracts. A loader thread reads contracts from the BucketList snapshot and pushes Wasm blobs; N-1 compiler threads pop and compile them into the module cache. Uses condition variables for flow control with a byte-capacity buffer.

### `SorobanMetrics`

Aggregates ledger-wide and per-tx Soroban resource usage metrics (CPU instructions, read/write bytes/entries, event sizes, host function execution times). Uses atomic counters for thread-safe accumulation during parallel apply.

---

### `LedgerEntryScope<S>` / `ScopedLedgerEntry<S>`

Template-based compile-time + runtime scope-checking system for ledger entries. Prevents cross-thread and cross-phase access bugs.

**Static scopes** (enum `StaticLedgerEntryScope`): `GlobalParApply`, `ThreadParApply`, `TxParApply`, `LclSnapshot`, `HotArchive`, `RawBucket`.

**Scope transitions** (allowed adoptions): `GlobalParApply ↔ ThreadParApply`, `ThreadParApply ↔ TxParApply`.

Each scope has activation/deactivation to prevent stale reads from outer scopes. `DeactivateScopeGuard` provides RAII deactivation.

### `TrustLineWrapper` / `ConstTrustLineWrapper`

Safe wrappers for trust line operations. Handles the special case of issuer accounts (which have no actual trust line entry but behave as if they have infinite balance). Provides `getBalance()`, `addBalance()`, `getBuyingLiabilities()`, `getSellingLiabilities()`, `addBuyingLiabilities()`, `addSellingLiabilities()`, `isAuthorized()`, etc.

### `RestoredEntries`

Tracks entries restored during a ledger, in two categories: `hotArchive` (restored from hot archive BL, involves IO) and `liveBucketList` (restored from live BL where TTL expired but wasn't evicted, just TTL update). Maps `LedgerKey → LedgerEntry`.

### `LedgerRange` / `CheckpointRange`

Simple half-open range types. `LedgerRange(first, count)` for ledger sequences. `CheckpointRange(first, count, frequency)` for history checkpoints, with methods to convert between checkpoint and ledger ranges.

---

## Key Control Flow

### Ledger Close (Normal Path)

1. **Herder** calls `LedgerManager::valueExternalized()` with `LedgerCloseData` containing the consensus tx set.
2. `LedgerApplyManager` queues the ledger and posts to the apply thread.
3. Apply thread calls `LedgerManagerImpl::applyLedger()`:
   a. Finishes any pending Wasm compilation.
   b. Marks phase `APPLYING`.
   c. Opens a `LedgerTxn` on `LedgerTxnRoot`, loads header, validates tx set hash.
   d. **Prefetches** source account entries.
   e. **Processes fees/seqnums** (`processFeesSeqNums`): charges fees, increments sequence numbers.
   f. **Applies transactions** (`applyTransactions`): dispatches to sequential phase (classic) and parallel phase (Soroban). Classic txs are applied serially. Soroban txs are grouped into stages/clusters and executed in parallel on worker threads.
   g. Marks phase `COMMITTING`.
   h. **Applies upgrades** from SCP value.
   i. **Seals** the LedgerTxn, writes to BucketList and DB, runs invariants (`sealLedgerTxnAndStoreInBucketsAndDB`).
   j. Emits ledger close meta if streaming is enabled.
   k. **Commits** the SQL transaction.
   l. Starts background eviction scan for next ledger.
   m. Marks phase `READY_TO_APPLY` (end of committing).
   n. Posts `advanceLedgerStateAndPublish` back to main thread (or calls directly if on main thread).
4. Main thread in `advanceLedgerStateAndPublish`:
   a. Publishes any pending history checkpoint.
   b. GCs unreferenced buckets.
   c. Updates LCL state.
   d. Triggers next ledger in Herder.

### Parallel Soroban Apply

Soroban transactions are organized into **stages**, each containing independent **clusters** of transactions. Within a stage:
1. `applySorobanStage` creates a `GlobalParallelApplyLedgerState` from the current LedgerTxn state.
2. `applySorobanStageClustersInParallel` spawns worker threads, each running `applyThread` on one cluster.
3. Worker threads execute transactions using read-only snapshots of ledger state. Changes are accumulated but not committed.
4. After all threads complete, results are merged back by the primary apply thread.

### LedgerTxn Commit Flow

When a `LedgerTxn` commits:
- If parent is another `LedgerTxn`: entries from `mEntry` are merged into parent's `mEntry` via `commitChild`. Parent's `mWorstBestOffer` and `mRestoredEntries` are updated.
- If parent is `LedgerTxnRoot`: opens a SQL transaction, iterates all entries via `EntryIterator`, applies bulk SQL operations (inserts to BucketList, upserts/deletes for offers in SQL), clears the entry cache.

---

## Ownership Relationships

- `Application` owns one `LedgerManagerImpl` and one `LedgerTxnRoot`.
- `LedgerManagerImpl` owns `ApplyState` (which owns `InMemorySorobanState`, `SorobanModuleCache`, `SharedModuleCacheCompiler`, `LedgerApplyMetrics`) and `CompleteConstLedgerStatePtr` (LCL state).
- `LedgerTxnRoot` holds a reference to `InMemorySorobanState` (owned by `ApplyState`), owns `EntryCache`, `BestOffers` cache, and the SQL `SessionWrapper`.
- `LedgerTxn` holds a reference to its parent (`AbstractLedgerTxnParent&`) and owns its `Impl` (which owns `EntryMap`, `ActiveMap`, `MultiOrderBook`, `WorstBestOfferMap`).
- `LedgerTxnEntry` weakly references its `Impl` (which references back to the owning `LedgerTxn`).
- `CompleteConstLedgerState` owns immutable snapshots of BucketList, hot archive, Soroban config, header, and HAS.

## Key Data Flows

1. **Consensus → Apply:** `LedgerCloseData` (tx set + hash) flows from Herder/SCP → `LedgerApplyManager` → apply thread → `LedgerManagerImpl::applyLedger()`.
2. **Apply → BucketList:** `LedgerTxn::getAllEntries()` extracts init/live/dead entries → `BucketManager::addLiveBatch()` and `addHotArchiveBatch()`.
3. **Apply → SQL:** `LedgerTxnRoot::commitChild()` writes offer changes to SQL via bulk operations; `LedgerHeaderUtils::storeInDatabase()` persists ledger headers.
4. **Apply → InMemorySorobanState:** After sealing the LedgerTxn, `ApplyState::updateInMemorySorobanState()` applies init/live/dead entries to the in-memory Soroban cache.
5. **Apply → Meta Stream:** `LedgerCloseMetaFrame` accumulates per-tx meta during apply, then is emitted via `mMetaStream`.
6. **Apply → Main Thread:** `CompleteConstLedgerState` (new LCL) is posted from apply thread to main thread via `advanceLedgerStateAndPublish()`.
7. **Startup/Catchup → State:** `loadLastKnownLedger()` or `setLastClosedLedger()` initializes LCL state, populates `InMemorySorobanState` from BucketList snapshot, and compiles the Soroban module cache.
