---
name: subsystem-summary-of-history
description: "read this skill for a token-efficient summary of the history subsystem"
---

# History Subsystem Technical Summary

## Overview

The history subsystem is responsible for storing and retrieving historical records in long-term public archives. It handles two forms of data:

1. **Buckets** from the BucketList — checkpoints of full ledger state.
2. **History blocks** — sequential logs of ledger headers, transactions, and transaction results.

Checkpoints occur every 64 ledgers (or 8 when `ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING` is set). A checkpoint is identified by the last ledger in its range. The first checkpoint covers ledgers [1, 63]; subsequent ones cover [K*64, (K+1)*64 - 1]. Archives are served over HTTP with the root state at `.well-known/stellar-history.json`. Per-checkpoint files use 3-level hex directory prefixes (e.g., `ledger/12/34/56/ledger-0x12345677.xdr.gz`).

---

## Key Classes and Data Structures

### `HistoryManager` (abstract base, `HistoryManager.h`)

Abstract interface for the history subsystem. Provides:

- **Static checkpoint arithmetic**: `checkpointContainingLedger()`, `firstLedgerInCheckpointContaining()`, `isLastLedgerInCheckpoint()`, `sizeOfCheckpointContaining()`, `getCheckpointFrequency()`, etc. All take `Config const&` and compute checkpoint boundaries from ledger numbers.
- **Publish queue management** (static): `publishQueuePath()`, `publishQueueLength()`, `getMinLedgerQueuedToPublish()`, `getMaxLedgerQueuedToPublish()`, `getPublishQueueStates()`, `getBucketsReferencedByPublishQueue()`, `deletePublishedFiles()`.
- **Virtual interface** implemented by `HistoryManagerImpl`:
  - `maybeQueueHistoryCheckpoint()` — conditionally queues a checkpoint at the right ledger boundary.
  - `queueCurrentHistory()` — snapshots bucket list state and writes a checkpoint file for the publish queue.
  - `publishQueuedHistory()` — picks the oldest queued checkpoint and initiates publication.
  - `maybeCheckpointComplete()` — finalizes checkpoint files after ledger commit.
  - `appendTransactionSet()` / `appendLedgerHeader()` — delegates to `CheckpointBuilder` for incremental checkpoint construction.
  - `restoreCheckpoint()` — crash-recovery: cleans up and restores checkpoint state on startup.
  - `historyPublished()` — callback when publication succeeds/fails; cleans up published files and triggers next publish.
  - `waitForCheckpointPublish()` — blocking wait (testing/load-generation only).

Contains the `LedgerVerificationStatus` enum used during catchup verification.

### `HistoryManagerImpl` (`HistoryManagerImpl.h`, `HistoryManagerImpl.cpp`)

Concrete implementation of `HistoryManager`. Key members:

- `mApp` — reference to the `Application`.
- `mWorkDir` — `std::unique_ptr<TmpDir>` for temporary file staging.
- `mPublishWork` — `std::shared_ptr<BasicWork>`, the currently-running publish work (only one active at a time).
- `mPublishQueued` — `std::atomic<int>` counter of enqueued checkpoints.
- `mPublishSuccess` / `mPublishFailure` — `medida::Meter` for publish metrics.
- `mEnqueueToPublishTimer` — `medida::Timer` measuring enqueue-to-publish latency.
- `mEnqueueTimes` — `UnorderedMap<uint32_t, time_point>` mapping checkpoint ledger to enqueue time.
- `mCheckpointBuilder` — `CheckpointBuilder` instance (owned by value).

**Key methods:**

- `queueCurrentHistory(ledger, ledgerVers)`: Snapshots the `LiveBucketList` (and optionally `HotArchiveBucketList`), constructs a `HistoryArchiveState`, serializes it to a temporary `.checkpoint.dirty` file in the publish queue directory. Does NOT finalize (rename) yet — that happens after ledger commit.
- `takeSnapshotAndPublish(has)`: Creates a `StateSnapshot`, then schedules a 3-phase work pipeline: `ResolveSnapshotWork` → `WriteSnapshotWork` → `PutSnapshotFilesWork`, wrapped in a `ConditionalWork` with a configurable delay (`PUBLISH_TO_ARCHIVE_DELAY`). Only one publish operation runs at a time (`mPublishWork` guards this).
- `publishQueuedHistory()`: Loads the oldest `.checkpoint` file from the publish queue directory and calls `takeSnapshotAndPublish`.
- `maybeCheckpointComplete(lcl)`: Calls `mCheckpointBuilder.checkpointComplete(lcl)` to rename dirty data files to final names, then renames the `.checkpoint.dirty` queue file to `.checkpoint`.
- `historyPublished(ledgerSeq, buckets, success)`: On success, records timing metrics, calls `deletePublishedFiles()` to clean up local copies, resets `mPublishWork`, and posts `publishQueuedHistory()` to the main thread to process the next checkpoint.
- `restoreCheckpoint(lcl)`: Calls `mCheckpointBuilder.cleanup(lcl)`, removes stale tmp checkpoint queue files with `seq > lcl`, and finalizes any checkpoint at `lcl` boundary.

### `CheckpointBuilder` (`CheckpointBuilder.h`, `CheckpointBuilder.cpp`)

Manages ACID-transactional appending of confirmed ledgers to sequential XDR streams during checkpoint construction. Owned by `HistoryManagerImpl`.

**Key members:**

- `mTxResults`, `mTxs`, `mLedgerHeaders` — `std::unique_ptr<XDROutputFileStream>`, the three data streams for the current in-progress checkpoint. Written as `.dirty` files (temporary).
- `mOpen` — whether streams are currently open.
- `mStartupValidationComplete` — guards against appending before `cleanup()` is called on startup.
- `mSkipFirstCheckpointSinceItIsIncomplete` — set if a node enabled publishing mid-checkpoint.

**Key methods:**

- `ensureOpen(ledgerSeq)`: Opens the three XDR output streams for the checkpoint containing `ledgerSeq`. Files are named using `FileTransferInfo` with `.dirty` suffix. Streams use `fsync` for durability.
- `appendTransactionSet(ledgerSeq, txSet, resultSet)`: Serializes transactions and results to their respective streams. Only writes if there are non-empty results. Checks startup validation.
- `appendLedgerHeader(header)`: Serializes a `LedgerHeaderHistoryEntry` (header + hash) to the ledger header stream.
- `checkpointComplete(checkpoint)`: Closes all streams, then renames each `.dirty` file to its final canonical name via `fs::durableRename`. This is the "commit" step for checkpoint data files.
- `cleanup(lcl)`: Startup crash recovery. For each of the three file types: if the final file exists, just deletes the next checkpoint's dirty files. If only a dirty file exists, truncates it to entries ≤ `lcl` (handling partial writes via XDR deserialization errors). Validates that the ledger header file ends exactly at `lcl`. Sets `mStartupValidationComplete = true`.

**Atomicity model:** Files are written as `.dirty` temporaries and renamed after ledger commit. This guarantees:
- Dirty files always end at a ledger ≥ LCL in DB.
- Final files always end at a ledger ≤ LCL in DB.
- On crash, `cleanup()` can truncate dirty files to match committed state.

### `HistoryArchiveState` (`HistoryArchive.h`, `HistoryArchive.cpp`)

A snapshot of a ledger number and its associated bucket list state. Used both for publication to archives and for persisting local BucketList state. Serialized as JSON (cereal).

**Key fields:**

- `version` — 1 (before hot archive) or 2 (with hot archive).
- `server` — stellar-core version string.
- `networkPassphrase` — required for version 2.
- `currentLedger` — the ledger sequence this state describes.
- `currentBuckets` — `vector<HistoryStateBucket<LiveBucket>>`, one per BucketList level.
- `hotArchiveBuckets` — `vector<HistoryStateBucket<HotArchiveBucket>>`, present in version 2.

**Key methods:**

- `getBucketListHash()` — computes cumulative SHA256 hash matching the live BucketList algorithm.
- `differingBuckets(other)` — returns `BucketHashReturnT` (separate live/hot vectors) of bucket hashes needed to turn `other` into `this`. Used to determine which buckets to upload.
- `allBuckets()` — returns all referenced bucket hashes (curr, snap, and future hashes).
- `containsValidBuckets(app)` — validates structural integrity: correct level count, monotonic bucket versions, correct future-bucket state based on protocol version.
- `prepareForPublish(app)` — reconstitutes `FutureBucket` merge operations if needed for publication.
- `resolveAllFutures()` / `resolveAnyReadyFutures()` — resolve pending bucket merges (may block).
- `save()` / `load()` / `toString()` / `fromString()` — JSON serialization via cereal.

### `HistoryStateBucket<BucketT>` (`HistoryArchive.h`)

Template struct parameterized on bucket type (`LiveBucket` or `HotArchiveBucket`). Represents one level of the bucket list:

- `curr` — hash string of the current bucket.
- `snap` — hash string of the snapshot bucket.
- `next` — `FutureBucket<BucketT>`, representing an in-progress merge.

### `HistoryArchive` (`HistoryArchive.h`, `HistoryArchive.cpp`)

Represents a single configured history archive with shell-command-based get/put/mkdir operations.

- `hasGetCmd()` / `hasPutCmd()` / `hasMkdirCmd()` — capability checks.
- `getFileCmd(remote, local)` / `putFileCmd(local, remote)` / `mkdirCmd(remoteDir)` — format shell commands from config templates using `fmt::format`.
- Wraps `HistoryArchiveConfiguration` from Config.

### `HistoryArchiveManager` (`HistoryArchiveManager.h`, `HistoryArchiveManager.cpp`)

Manages the set of configured history archives. Owned by `Application`.

- Constructs `HistoryArchive` objects from `Config::HISTORY` entries.
- `checkSensibleConfig()` — validates archive configs (warns about read-only, write-only, inert archives).
- `selectRandomReadableHistoryArchive()` — picks a random archive for catchup, preferring read-only archives over read-write ones.
- `publishEnabled()` — returns true if any archive has both get and put commands.
- `getWritableHistoryArchives()` — returns archives with both get and put.
- `getHistoryArchive(name)` — lookup by name.
- `initializeHistoryArchive(arch)` — writes initial (empty) HAS to `well-known/stellar-history.json`.
- `getHistoryArchiveReportWork()` — creates work to fetch and report state from all archives.
- `getCheckLedgerHeaderWork(lhhe)` — creates work to verify a ledger header against all archives.

### `StateSnapshot` (`StateSnapshot.h`, `StateSnapshot.cpp`)

A point-in-time snapshot of checkpoint data ready for publication. Created by `takeSnapshotAndPublish`.

**Key members:**

- `mLocalState` — `HistoryArchiveState` being published.
- `mSnapDir` — `TmpDir` for staging SCP message files.
- `mLedgerSnapFile`, `mTransactionSnapFile`, `mTransactionResultSnapFile`, `mSCPHistorySnapFile` — `shared_ptr<FileTransferInfo>` for the four history file types.

**Key methods:**

- Constructor: Creates `FileTransferInfo` objects for each file type. Ledger/transaction/result files point to the publish history directory; SCP file uses a temporary directory.
- `writeSCPMessages()` — streams SCP history entries from the database into an XDR file for the checkpoint range.
- `differingHASFiles(other)` — returns the list of files (ledger, tx, results, SCP, plus differing bucket files) that need to be uploaded to bring `other` up to `this` state.

### `FileTransferInfo` (`FileTransferInfo.h`, `FileTransferInfo.cpp`)

Encapsulates naming conventions for history files (buckets, ledgers, transactions, results, SCP). Provides:

- Local paths (with/without `.gz`, `.dirty`, `.gz.tmp` suffixes).
- Remote paths (for archive upload).
- Base names following the pattern `{type}-{hexDigits}.xdr[.gz]`.
- Multiple constructors: from a bucket object (hash-based naming), from `TmpDir` + checkpoint ledger, or from `Config` + checkpoint ledger (publish directory).

### `FileType` enum (`FileTransferInfo.h`)

`HISTORY_FILE_TYPE_BUCKET`, `HISTORY_FILE_TYPE_LEDGER`, `HISTORY_FILE_TYPE_TRANSACTIONS`, `HISTORY_FILE_TYPE_RESULTS`, `HISTORY_FILE_TYPE_SCP`.

### `HistoryArchiveReportWork` (`HistoryArchiveReportWork.h`, `HistoryArchiveReportWork.cpp`)

A `WorkSequence` that fetches `HistoryArchiveState` from all configured archives and logs their status. Used for diagnostic reporting (`--report-last-history-checkpoint`).

### `HistoryUtils` (`HistoryUtils.h`, `HistoryUtils.cpp`)

Template function `getHistoryEntryForLedger<T>()` — advances through an XDR input stream to find a `TransactionHistoryEntry` or `TransactionHistoryResultEntry` matching a target ledger sequence. Handles gaps in history (empty ledgers with no transactions). Returns true if the target entry is found.

---

## Key Data Flows

### Checkpoint Construction (Ledger Close Path)

1. During `LedgerManagerImpl::closeLedger`, before commit:
   - `HistoryManager::appendTransactionSet()` → `CheckpointBuilder::appendTransactionSet()` writes tx and result XDR entries to `.dirty` streams.
   - `HistoryManager::appendLedgerHeader()` → `CheckpointBuilder::appendLedgerHeader()` writes ledger header entry.
2. If this ledger completes a checkpoint (`isLastLedgerInCheckpoint`):
   - `HistoryManager::maybeQueueHistoryCheckpoint()` is called. It snapshots the BucketList into a `HistoryArchiveState` and writes a `.checkpoint.dirty` file to the publish queue directory.
3. After ledger commit:
   - `HistoryManager::maybeCheckpointComplete()` renames `.dirty` data files to final names via `CheckpointBuilder::checkpointComplete()`, and renames the `.checkpoint.dirty` queue file to `.checkpoint`.
4. `HistoryManager::publishQueuedHistory()` picks the oldest `.checkpoint` file, loads the HAS, and calls `takeSnapshotAndPublish()`.

### Publication Pipeline

`takeSnapshotAndPublish` creates a `StateSnapshot` and schedules a 3-phase `WorkSequence`:

1. **`ResolveSnapshotWork`** — resolves any pending `FutureBucket` merges so all bucket hashes are concrete.
2. **`WriteSnapshotWork`** — writes SCP messages to disk (from DB), gzips all checkpoint files (ledger, tx, results, SCP).
3. **`PutSnapshotFilesWork`** — uploads files to all writable archives using shell put/mkdir commands.

The pipeline is wrapped in `ConditionalWork` with a configurable delay (`PUBLISH_TO_ARCHIVE_DELAY`). On completion, `historyPublished()` is called which cleans up local files and triggers the next queued publish.

### Crash Recovery (Startup Path)

1. `HistoryManager::restoreCheckpoint(lcl)` is called on startup with the last committed ledger.
2. `CheckpointBuilder::cleanup(lcl)`:
   - For each file type (results, transactions, ledger headers): if a dirty file exists, truncates it to entries ≤ lcl by reading/rewriting valid entries. Handles partial writes (XDR parse errors).
   - Deletes any dirty files for checkpoints beyond `lcl`.
   - If no files exist at all, sets `mSkipFirstCheckpointSinceItIsIncomplete`.
3. Stale `.checkpoint.dirty` queue files with `seq > lcl` are deleted.
4. If `lcl` is at a checkpoint boundary, `maybeCheckpointComplete` finalizes any unfinalised checkpoint.

### Catchup (Download Path)

Catchup is orchestrated by the `CatchupWork` family (in `historywork/`, outside this directory) and the `LedgerManager`. The history subsystem provides:

- `HistoryArchiveManager::selectRandomReadableHistoryArchive()` to pick a source.
- `HistoryArchiveState::differingBuckets()` to determine which buckets need downloading.
- `getHistoryEntryForLedger()` to iterate through downloaded tx/result history files.
- Checkpoint arithmetic functions to compute ranges.

---

## Publish Queue Storage

The publish queue is stored on the filesystem under `{BUCKET_DIR_PATH}/publishqueue/`. Each queued checkpoint is a binary-cereal–serialized `HistoryArchiveState` file named `{hex_ledger}.checkpoint`. Temporary files use `.checkpoint.dirty` suffix. The queue is scanned on startup and files are processed oldest-first.

---

## Threading Model

- All history operations run on the **main thread** (enforced by `releaseAssert(threadIsMain())` in `takeSnapshotAndPublish`).
- Publication uses the **Work/WorkScheduler** framework for async I/O (shell commands for get/put/mkdir) without blocking the main thread.
- `CheckpointBuilder` writes are synchronous within ledger close (on the main thread), using `fsync` for durability.
- `BucketList` snapshots (`getLiveBucketList()`) are taken on the main thread since only one thread modifies the bucket list.

---

## Ownership Relationships

```
Application
├── HistoryManagerImpl (unique_ptr via HistoryManager::create)
│   ├── CheckpointBuilder (value member)
│   │   ├── XDROutputFileStream mTxResults (unique_ptr, open during checkpoint)
│   │   ├── XDROutputFileStream mTxs (unique_ptr)
│   │   └── XDROutputFileStream mLedgerHeaders (unique_ptr)
│   ├── TmpDir mWorkDir (unique_ptr, lazy-initialized)
│   ├── BasicWork mPublishWork (shared_ptr, current publish job)
│   └── Metrics (references to medida meters/timers)
├── HistoryArchiveManager (value member in Application)
│   └── vector<shared_ptr<HistoryArchive>> mArchives
└── WorkScheduler (schedules publish Work items)
    └── ConditionalWork → PublishWork
        └── WorkSequence: ResolveSnapshotWork → WriteSnapshotWork → PutSnapshotFilesWork
            └── StateSnapshot (shared_ptr, passed through pipeline)
                ├── HistoryArchiveState mLocalState
                ├── TmpDir mSnapDir
                └── FileTransferInfo (shared_ptr) × 4 file types
```

---

## Key Constants

- `MAX_HISTORY_ARCHIVE_BUCKET_SIZE` = 100 GB — DOS protection limit on downloaded bucket size.
- Default checkpoint frequency = 64 ledgers (~5m20s at 5s close time).
- `HISTORY_ARCHIVE_STATE_VERSION_BEFORE_HOT_ARCHIVE` = 1, `HISTORY_ARCHIVE_STATE_VERSION_WITH_HOT_ARCHIVE` = 2.
- Archive root state file: `.well-known/stellar-history.json`.
