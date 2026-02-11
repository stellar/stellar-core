---
name: subsystem-summary-of-bucket
description: "read this skill for a token-efficient summary of the bucket subsystem"
---

# Bucket Subsystem Technical Summary

## Overview

The bucket subsystem implements a **log-structured merge tree (LSM-tree)** data structure called the **BucketList**. It maintains a canonical, hash-verifiable representation of all ledger state. There are two BucketList instances: the **LiveBucketList** (current ledger state) and the **HotArchiveBucketList** (recently evicted entries). Each is organized into 11 temporal levels (level 0 being youngest/smallest, level 10 being oldest/largest), where older levels are exponentially larger and change less frequently.

The system is designed to:
1. Provide a single canonical hash of all ledger entries without rehashing the entire database on each ledger close.
2. Enable efficient "catch-up" via incremental bucket downloads from history archives.
3. Support point and bulk lookups of ledger entries via indexed bucket files (BucketListDB).

## Key Classes and Data Structures

### Bucket Types (CRTP Hierarchy)

- **`BucketBase<BucketT, IndexT>`** — Abstract CRTP base for immutable, sorted, hashed containers of XDR entries. Holds a filename, hash, size, and optional index (`shared_ptr<IndexT>`). Provides the core `merge()` and `mergeInternal()` static methods. Buckets are designed to be held in `shared_ptr` and shared across threads.

- **`LiveBucket`** — Stores `BucketEntry` (INITENTRY, LIVEENTRY, DEADENTRY, METAENTRY). Supports shadows, INIT/DEAD annihilation, and in-memory level-0 merges. Has an optional `mEntries` vector for in-memory-only buckets. Index type: `LiveBucketIndex`. Key methods:
  - `fresh()` — Creates a new bucket from init/live/dead entry vectors, sorts, hashes, writes to disk.
  - `freshInMemoryOnly()` — Creates a bucket that only exists in memory (for level-0 "snap" that immediately merges).
  - `mergeInMemory()` — Merges two in-memory buckets without `FutureBucket`, used only for level 0.
  - `merge()` (inherited) — File-based merge of two buckets with optional shadows.
  - `maybePut()` — Shadow-aware write: elides entries shadowed by newer buckets (pre-v12), preserves INIT/DEAD lifecycle entries (v11+).
  - `mergeCasesWithEqualKeys()` — Handles INIT/DEAD annihilation and INIT+LIVE→INIT promotion.
  - `convertToBucketEntry()` — Converts raw ledger entry vectors into sorted `BucketEntry` vector.
  - `isTombstoneEntry()` — Returns true for DEADENTRY.

- **`HotArchiveBucket`** — Stores `HotArchiveBucketEntry` (HOT_ARCHIVE_ARCHIVED, HOT_ARCHIVE_LIVE, HOT_ARCHIVE_METAENTRY). No shadow support. Index type: `HotArchiveBucketIndex`. HOT_ARCHIVE_LIVE acts as tombstone (restored entries). Key methods:
  - `fresh()` — Creates bucket from archived entries and restored keys.
  - `maybePut()` — Always writes (no shadow logic).
  - `isTombstoneEntry()` — Returns true for HOT_ARCHIVE_LIVE.

### BucketList Structure

- **`BucketListBase<BucketT>`** — Abstract templated base for BucketList data structure. Contains a vector of `BucketLevel<BucketT>`. Defines the temporal-leveling algorithm: level sizes are powers of 4 (`levelSize(i) = 4^(i+1)`), each split into curr and snap halves. Key methods:
  - `addBatchInternal()` — Main entry point: adds a batch of entries at a ledger close. Walks levels top-down, calling `snap()` and `prepare()` on levels that should spill. Level 0 uses `prepareFirstLevel()` for in-memory merges.
  - `levelShouldSpill()` — Returns true when a level needs to snapshot curr→snap and merge snap into the next level.
  - `restartMerges()` — Re-starts merges after deserialization (catchup or restart). For v12+ merges, reconstructs from current BucketList state; for older merges, uses serialized hashes.
  - `resolveAnyReadyFutures()` — Non-blocking resolution of completed merges.
  - `getHash()` — Returns concatenated hash of all level hashes (each level = hash of curr + snap).
  - Static methods: `levelSize()`, `levelHalf()`, `sizeOfCurr()`, `sizeOfSnap()`, `oldestLedgerInCurr()`, `oldestLedgerInSnap()`, `keepTombstoneEntries()`, `bucketUpdatePeriod()`.

- **`BucketLevel<BucketT>`** — A single level in the BucketList. Holds `mCurr`, `mSnap` (both `shared_ptr<BucketT>`), and `mNextCurr` (a `std::variant<FutureBucket<BucketT>, shared_ptr<BucketT>>`). Key methods:
  - `prepare()` — Starts an async merge via `FutureBucket` (used for levels 1+).
  - `prepareFirstLevel()` — Specialization for level 0: does an in-memory merge if possible (`LiveBucket::mergeInMemory`), falls back to `prepare()` otherwise.
  - `commit()` — Resolves any pending merge and sets result as new curr.
  - `snap()` — Moves curr to snap, resets curr to empty bucket.

- **`LiveBucketList`** — Extends `BucketListBase<LiveBucket>`. Adds eviction-related methods (`updateStartingEvictionIterator`, `updateEvictionIterAndRecordStats`, `checkIfEvictionScanIsStuck`) and `addBatch()` which calls `addBatchInternal()` with init/live/dead entry vectors. Also `maybeInitializeCaches()` for index random eviction caches.

- **`HotArchiveBucketList`** — Extends `BucketListBase<HotArchiveBucket>`. Simpler `addBatch()` with archived/restored entry vectors.

### BucketManager

- **`BucketManager`** — Singleton owner of the BucketList instances, bucket files, and merge futures. Thread-safe for bucket file operations via `mBucketMutex`. Key responsibilities:
  - **Bucket file management**: `adoptFileAsBucket()` moves temp files into the bucket directory, deduplicating by hash. `forgetUnreferencedBuckets()` GCs unreferenced buckets. `cleanupStaleFiles()` removes orphaned files.
  - **Merge future tracking**: `mLiveBucketFutures` / `mHotArchiveBucketFutures` (hash maps of `MergeKey → shared_future`) track running merges. `mFinishedMerges` (`BucketMergeMap`) records completed merge input→output mappings for reattachment.
  - **Batch ingestion**: `addLiveBatch()` and `addHotArchiveBatch()` feed new entries from ledger close into the BucketList.
  - **Snapshotting**: `snapshotLedger()` computes the `bucketListHash` for the LedgerHeader.
  - **Eviction**: `startBackgroundEvictionScan()` launches async eviction scan on a snapshot; `resolveBackgroundEvictionScan()` applies results.
  - **State management**: `assumeState()` loads BucketList from a `HistoryArchiveState`. `loadCompleteLedgerState()` materializes the full ledger into a map.
  - **Index management**: `maybeSetIndex()` sets the index for a bucket, handling race conditions on startup.
  - Owns: `LiveBucketList`, `HotArchiveBucketList`, `BucketSnapshotManager`, `TmpDirManager`, bucket maps (`mSharedLiveBuckets`, `mSharedHotArchiveBuckets`), merge future maps, `BucketMergeMap`.

### Merge Infrastructure

- **`FutureBucket<BucketT>`** — Wraps a `std::shared_future<shared_ptr<BucketT>>` representing an in-progress or completed merge. Has 5 states: `FB_CLEAR`, `FB_HASH_OUTPUT`, `FB_HASH_INPUTS`, `FB_LIVE_OUTPUT`, `FB_LIVE_INPUTS`. Serializable via cereal (saves/loads hash strings). Key lifecycle:
  1. Constructed with live inputs → `FB_LIVE_INPUTS`, immediately calls `startMerge()`.
  2. `startMerge()` checks for existing merge via `BucketManager::getMergeFuture()` (reattachment). If none, creates a `packaged_task` that calls `BucketT::merge()` and posts it to a background worker thread.
  3. `mergeComplete()` polls the future. `resolve()` blocks to get result → `FB_LIVE_OUTPUT`.
  4. Serialized state can be `FB_HASH_INPUTS` or `FB_HASH_OUTPUT`. `makeLive()` reconstitutes from hashes.

- **`MergeKey`** — Identifies a merge by its inputs: `keepTombstoneEntries`, `inputCurrHash`, `inputSnapHash`, `shadowHashes`. Used as key in merge future/finished maps.

- **`BucketMergeMap`** — Bidirectional weak mapping of merge input→output and output→input. Stores `MergeKey→Hash`, `Hash→Hash` (input→output multimap), and `Hash→MergeKey` (output→input multimap). Used for merge reattachment: if a merge's output bucket still exists, we can synthesize a pre-resolved future instead of re-running the merge.

- **`MergeInput<BucketT>`** (abstract), **`FileMergeInput<BucketT>`**, **`MemoryMergeInput<BucketT>`** — Adapters providing a uniform interface over either `BucketInputIterator` pairs (file-based merge) or in-memory `vector<EntryT>` pairs. Methods: `isDone()`, `oldFirst()`, `newFirst()`, `equalKeys()`, `getOldEntry()`, `getNewEntry()`, `advanceOld()`, `advanceNew()`.

### I/O Iterators

- **`BucketInputIterator<BucketT>`** — Reads entries sequentially from a bucket file via `XDRInputFileStream`. Auto-extracts the leading METAENTRY. Provides `operator*`, `operator++`, `pos()`, `seek()`, `size()`.

- **`BucketOutputIterator<BucketT>`** — Writes entries to a temp file, computing a running SHA256 hash. `put()` buffers one entry to deduplicate adjacent same-key entries. `getBucket()` finalizes the file, calls `BucketManager::adoptFileAsBucket()`, and returns the new bucket. Respects `keepTombstoneEntries` to elide tombstones at the bottom level. Writes a METAENTRY at the start if protocol version ≥ 11.

### Snapshot & Query Layer (BucketListDB)

- **`BucketListSnapshotData<BucketT>`** — Immutable snapshot of a BucketList: a vector of `Level{curr, snap}` (shared_ptr to const buckets) plus a `LedgerHeader`. Thread-safe to share.

- **`SearchableBucketListSnapshot<BucketT>`** — Provides lookup functionality over a snapshot. Each instance owns mutable file stream caches (`mStreams`) for I/O. Key methods:
  - `load(LedgerKey)` — Point lookup: iterates buckets newest-to-oldest, returns first match via index lookup + file read. Returns the `LoadT` (LedgerEntry for live, HotArchiveBucketEntry for hot archive).
  - `loadKeysFromBucket()` — Bulk scan: uses index `scan()` iterator for sequential multi-key lookup within a bucket.
  - `loadKeysInternal()` — Loads keys from all buckets, supports historical snapshots.
  - `loopAllBuckets()` — Iterates all non-empty bucket (curr, snap) across levels, calling a function. Stops early on `Loop::COMPLETE`.
  - `getBucketEntry()` — Single-key lookup via index: CACHE_HIT returns cached entry, FILE_OFFSET reads from disk, NOT_FOUND skips.

- **`SearchableLiveBucketListSnapshot`** — Extends the base with live-specific queries:
  - `loadKeys()` — Bulk load with timer.
  - `loadPoolShareTrustLinesByAccountAndAsset()` — Two-step query: index lookup for PoolIDs, then bulk trustline load.
  - `loadInflationWinners()` — Legacy inflation vote counting.
  - `scanForEviction()` — Background eviction scan: iterates bucket region, collects expired entries.
  - `scanForEntriesOfType()` — Iterates entries of a given `LedgerEntryType` using type range bounds.

- **`SearchableHotArchiveBucketListSnapshot`** — Hot archive queries: `loadKeys()`, `scanAllEntries()`.

- **`BucketSnapshotManager`** — Thread-safe boundary between main-thread BucketList mutations and read-only snapshots. Holds canonical snapshots behind a `SharedMutex`. Key methods:
  - `updateCurrentSnapshot()` — Called by main thread after BucketList changes. Takes exclusive lock, rotates historical snapshots.
  - `copySearchableLiveBucketListSnapshot()` / `copySearchableHotArchiveBucketListSnapshot()` — Creates a new `Searchable*Snapshot` with fresh stream caches pointing to the current snapshot data.
  - `maybeCopySearchableBucketListSnapshot()` — Refreshes a snapshot only if a newer one is available (shared lock).
  - `maybeCopyLiveAndHotArchiveSnapshots()` — Atomically refreshes both live and hot archive snapshots for consistency.

### Index System

- **`LiveBucketIndex`** — Wraps either an `InMemoryIndex` (small buckets) or `DiskIndex<LiveBucket>` (large buckets), selected based on config (`BUCKETLIST_DB_INDEX_CUTOFF`). Additionally owns an optional `RandomEvictionCache` for ACCOUNT entries. Key methods:
  - `lookup(LedgerKey)` — Returns `IndexReturnT` (CACHE_HIT, FILE_OFFSET, or NOT_FOUND).
  - `scan(IterT, LedgerKey)` — Sequential scan for bulk loads.
  - `getPoolIDsByAsset()` — Returns PoolIDs for asset-based trustline queries.
  - `maybeInitializeCache()` — Lazily initializes the random eviction cache proportional to bucket's share of total accounts.
  - `typeNotSupported()` — Returns true for OFFER type (offers are loaded from SQL during catchup, not BucketListDB).
  - Version: `BUCKET_INDEX_VERSION = 6`.

- **`HotArchiveBucketIndex`** — Always uses `DiskIndex<HotArchiveBucket>` (no in-memory index, no cache). Version: `BUCKET_INDEX_VERSION = 0`.

- **`DiskIndex<BucketT>`** — Persisted range-based index. Contains:
  - `RangeIndex` (`vector<pair<RangeEntry, streamoff>>`) — Maps key ranges to file offsets (page boundaries).
  - `BinaryFuseFilter16` — Bloom-filter-like structure for quick negative lookups.
  - `AssetPoolIDMap` — Asset→PoolID mapping (LiveBucket only).
  - `BucketEntryCounters` — Per-type entry counts and sizes.
  - `typeRanges` — Map of `LedgerEntryType → (startOffset, endOffset)` for type-specific scans.
  - Persisted to disk via cereal. Loaded on startup if version/pageSize match.

- **`InMemoryIndex`** — For small buckets. Uses `InMemoryBucketState` (an `unordered_set<InternalInMemoryBucketEntry>`) to store all entries in memory. `InternalInMemoryBucketEntry` uses type-erasure to allow lookup by `LedgerKey` in a set of `BucketEntry` (C++20 heterogeneous lookup workaround).

- **`IndexReturnT`** — Variant return type from index queries: `IndexPtrT` (cache hit), `std::streamoff` (file offset), or `std::monostate` (not found).

- **`BucketIndexUtils`** — Free functions: `createIndex()` builds a new index from a bucket file; `loadIndex()` loads a persisted index from disk; `getPageSizeFromConfig()`.

### Comparison and Ordering

- **`LedgerEntryIdCmp`** — Compares `LedgerEntry` or `LedgerKey` by identity (type, then type-specific key fields). Used for sorted sets and merge ordering.

- **`BucketEntryIdCmp<BucketT>`** — Compares `BucketEntry` or `HotArchiveBucketEntry` by their embedded ledger keys. METAENTRY sorts below all others. Handles cross-type comparisons (LIVEENTRY vs DEADENTRY, ARCHIVED vs LIVE).

### Catchup Support

- **`BucketApplicator`** — Applies a `LiveBucket` to the database during history catchup. Processes entries in scheduler-friendly batches (`LEDGER_ENTRY_BATCH_COMMIT_SIZE`). Only applies offers (seeks to offer range using type index). Tracks `seenKeys` to avoid applying shadowed entries. Handles pre-v11 entries that lack INITENTRY.

### Eviction

- **`EvictionResultEntry`** — A single eviction candidate: the `LedgerEntry`, its `EvictionIterator` position, and `liveUntilLedger`.
- **`EvictionResultCandidates`** — Collection of eviction candidates from a background scan, with validity checks against archival settings.
- **`EvictedStateVectors`** — Final eviction output: `deletedKeys` (temp entries + TTLs) and `archivedEntries` (persistent entries).
- **`EvictionStatistics`** — Tracks eviction cycle metrics (entry age, cycle period).
- **`EvictionMetrics`** — Medida metrics for eviction (entries evicted, bytes scanned, blocking/background time).

### Utility Types

- **`MergeCounters`** — Fine-grained counters for merge operations (entry types processed, shadow elisions, reattachments, annihilations). Not published via medida; used for internal tracking and testing.
- **`BucketEntryCounters`** — Per-`LedgerEntryTypeAndDurability` counts and sizes. Stored in indexes, aggregated across buckets.
- **`LedgerEntryTypeAndDurability`** — Finer-grained enum distinguishing TEMPORARY vs PERSISTENT CONTRACT_DATA.

## Key Control Flows

### Ledger Close (addBatch)

1. `BucketManager::addLiveBatch()` → `LiveBucketList::addBatch()` → `addBatchInternal()`.
2. Walk levels top-down (10→1): if `levelShouldSpill(ledger, i-1)`, then `levels[i-1].snap()` + `levels[i].commit()` + `levels[i].prepare()`.
3. Level 0: `prepareFirstLevel()` — creates fresh in-memory bucket, merges with curr in-memory via `LiveBucket::mergeInMemory()`, commits immediately (synchronous, no background thread).
4. Levels 1+: `prepare()` creates a `FutureBucket` which launches a background merge task via `app.postOnBackgroundThread()`.
5. `resolveAnyReadyFutures()` non-blockingly resolves any completed merges.
6. `BucketSnapshotManager::updateCurrentSnapshot()` creates new immutable snapshot data.

### Background Merge (FutureBucket::startMerge)

1. Constructs a `MergeKey` from inputs. Checks `BucketManager::getMergeFuture()` for reattachment.
2. If no existing future, creates a `packaged_task` that calls `BucketT::merge()`.
3. `merge()` opens `BucketInputIterator`s on old and new buckets, creates `BucketOutputIterator`, then calls `mergeInternal()`.
4. `mergeInternal()` dispatches: if entries have different keys, the lesser key is accepted via `maybePut()`; if keys are equal, `mergeCasesWithEqualKeys()` handles INIT/DEAD annihilation, lifecycle promotion, and shadow checks.
5. `BucketOutputIterator::getBucket()` finalizes → `BucketManager::adoptFileAsBucket()` → file rename into bucket dir, index construction, merge tracking.

### Point Lookup (BucketListDB)

1. Caller obtains a `SearchableLiveBucketListSnapshot` from `BucketSnapshotManager`.
2. `load(LedgerKey)` iterates all buckets via `loopAllBuckets()`.
3. For each bucket, `getBucketEntry()` calls `index.lookup()` → returns CACHE_HIT (cached BucketEntry), FILE_OFFSET (seek + read page), or NOT_FOUND (skip bucket).
4. First non-null result wins (newer buckets shadow older ones).

### Eviction Scan

1. `BucketManager::startBackgroundEvictionScan()` posts a task to eviction background thread.
2. Task calls `SearchableLiveBucketListSnapshot::scanForEviction()`, which iterates through a region of the bucket file, collecting candidates whose TTL entries are expired.
3. Main thread calls `resolveBackgroundEvictionScan()`: validates candidates against current ledger state, evicts up to `maxEntriesToArchive`, updates eviction iterator in network config.

## Threading Model

- **Main thread**: Owns `BucketManager`, `LiveBucketList`, `HotArchiveBucketList`. Calls `addBatch()`, `snapshotLedger()`, `forgetUnreferencedBuckets()`. Updates canonical snapshots in `BucketSnapshotManager`.
- **Worker threads** (via `app.postOnBackgroundThread()`): Run `FutureBucket` merges. Call `BucketT::merge()`, `adoptFileAsBucket()`. Access `mBucketMutex` for file operations and future maps.
- **Eviction background thread**: Runs `scanForEviction()` on a snapshot.
- **Query threads** (Soroban/overlay): Use `SearchableBucketListSnapshot` copies from `BucketSnapshotManager`. Each snapshot has its own file stream cache. Snapshot data is immutable and shared via `shared_ptr`.
- **Synchronization**:
  - `mBucketMutex` (RecursiveMutex): Guards bucket file maps, future maps, finished merge map. Must be acquired AFTER `LedgerManagerImpl::mLedgerStateMutex`.
  - `mSnapshotMutex` (SharedMutex in `BucketSnapshotManager`): Exclusive for `updateCurrentSnapshot()`, shared for copying snapshots.
  - `mCacheMutex` (shared_mutex in `LiveBucketIndex`): Guards the random eviction cache.

## Ownership Relationships

```
BucketManager
├── LiveBucketList (unique_ptr)
│   └── vector<BucketLevel<LiveBucket>>
│       ├── mCurr: shared_ptr<LiveBucket>
│       │   ├── mFilename, mHash, mSize
│       │   ├── mIndex: shared_ptr<LiveBucketIndex const>
│       │   │   ├── DiskIndex<LiveBucket> (or InMemoryIndex)
│       │   │   └── RandomEvictionCache (optional)
│       │   └── mEntries: unique_ptr<vector<BucketEntry>> (level 0 only)
│       ├── mSnap: shared_ptr<LiveBucket>
│       └── mNextCurr: variant<FutureBucket<LiveBucket>, shared_ptr<LiveBucket>>
│           └── FutureBucket holds shared_future + input/output bucket refs
├── HotArchiveBucketList (unique_ptr)
│   └── vector<BucketLevel<HotArchiveBucket>> (same structure)
├── BucketSnapshotManager (unique_ptr)
│   ├── mCurrLiveSnapshot: shared_ptr<BucketListSnapshotData<LiveBucket>>
│   ├── mCurrHotArchiveSnapshot: shared_ptr<BucketListSnapshotData<HotArchiveBucket>>
│   └── historical snapshot maps
├── mSharedLiveBuckets: map<Hash, shared_ptr<LiveBucket>>
├── mSharedHotArchiveBuckets: map<Hash, shared_ptr<HotArchiveBucket>>
├── mLiveBucketFutures: map<MergeKey, shared_future<shared_ptr<LiveBucket>>>
├── mHotArchiveBucketFutures: map<MergeKey, shared_future<shared_ptr<HotArchiveBucket>>>
├── mFinishedMerges: BucketMergeMap
├── TmpDirManager (unique_ptr)
└── Config (copy, thread-safe)
```

## Key Data Flows

1. **Ledger close → BucketList**: `LedgerManager` → `BucketManager::addLiveBatch(initEntries, liveEntries, deadEntries)` → `LiveBucketList::addBatch()` → spill cascade through levels → background merges.

2. **BucketList → Snapshot**: After `addBatch()`, main thread calls `BucketSnapshotManager::updateCurrentSnapshot()` → creates new immutable `BucketListSnapshotData` from current BucketList state.

3. **Snapshot → Query**: Background threads call `BucketSnapshotManager::copySearchableLiveBucketListSnapshot()` → gets a `SearchableLiveBucketListSnapshot` with fresh file streams → `load()` / `loadKeys()` for point and bulk queries.

4. **Eviction flow**: Main thread → `startBackgroundEvictionScan()` → eviction thread scans snapshot → `resolveBackgroundEvictionScan()` on main thread applies evictions to `AbstractLedgerTxn` → evicted persistent entries flow to `addHotArchiveBatch()` → `HotArchiveBucketList`.

5. **Catchup flow**: `HistoryManager` downloads bucket files → `BucketManager::assumeState(HistoryArchiveState)` → sets curr/snap on each level, restarts merges → `BucketApplicator` applies offers to database.

6. **Merge reattachment**: On restart, `FutureBucket::makeLive()` reconstitutes from hashes → `startMerge()` checks `BucketManager::getMergeFuture()` → if finished merge exists in `BucketMergeMap`, synthesizes a pre-resolved future; otherwise re-launches background merge.
