---
name: subsystem-summary-of-historywork
description: "read this skill for a token-efficient summary of the historywork subsystem"
---

# Historywork Subsystem Technical Summary

The `historywork` subsystem implements the concrete work units (tasks) for stellar-core's history archive interactions. It provides the building blocks for publishing ledger history to archives and downloading/verifying history during catchup. All classes inherit from the `Work`/`BasicWork`/`BatchWork` framework defined in `src/work/`.

---

## Base Infrastructure

### `RunCommandWork` (RunCommandWork.h/cpp)
**Inherits:** `BasicWork`

Base class for all work units that execute external shell commands via `ProcessManager`. Subclasses override `getCommand()` to return a `CommandInfo` (command string + optional output file path). The work spawns a process, enters `WORK_WAITING`, and wakes up via an async callback on `ProcessExitEvent` when the process completes.

**Key functions:**
- `onRun()`: If not done, calls `getCommand()`, spawns a process via `mApp.getProcessManager().runProcess()`, and installs an async callback that sets `mDone`/`mEc` and calls `wakeUp()`.
- `onReset()`: Clears done state, error code, and exit event.
- `onAbort()`: Attempts `tryProcessShutdown()` on the running process.
- `getCommand()`: Pure virtual — returns `CommandInfo{command, outFile}`.

**Key data:**
- `mDone` (bool): Whether the process has exited.
- `mEc` (asio::error_code): Exit status of the process.
- `mExitEvent` (weak_ptr<ProcessExitEvent>): Handle to the running process.

### `CommandInfo` (RunCommandWork.h)
Simple struct holding `mCommand` (shell command string) and `mOutFile` (optional output file path for redirected output).

### `Progress` (Progress.h/cpp)
Utility function `fmtProgress(app, task, range, curr)` that formats a human-readable progress string like `"downloading ledger files 5/10 (50%)"` based on checkpoint frequency and a `LedgerRange`.

---

## File Transfer Operations (Low-level)

### `GetRemoteFileWork` (GetRemoteFileWork.h/cpp)
**Inherits:** `RunCommandWork`

Downloads a single file from a history archive. If no specific archive is provided (`mArchive == nullptr`), selects a random readable archive on each run/retry via `HistoryArchiveManager::selectRandomReadableHistoryArchive()`.

**Key functions:**
- `getCommand()`: Resolves the archive (random or specified), calls `mCurrentArchive->getFileCmd(remote, local)` to get the shell download command.
- `onSuccess()`: Records bytes downloaded to metrics.
- `onFailureRaise()`: Records failure metric and logs a warning identifying the archive.

**Key data:**
- `mRemote`, `mLocal`: Source and destination paths.
- `mArchive`: Fixed archive (or null for random selection).
- `mCurrentArchive`: The archive actually used for the current attempt.
- `mFailuresPerSecond`, `mBytesPerSecond`: Medida metrics.

### `PutRemoteFileWork` (PutRemoteFileWork.h/cpp)
**Inherits:** `RunCommandWork`

Uploads a single file to a history archive using `mArchive->putFileCmd(local, remote)`. Requires a non-null archive with put capability. Retries `RETRY_A_LOT`.

### `MakeRemoteDirWork` (MakeRemoteDirWork.h/cpp)
**Inherits:** `RunCommandWork`

Creates a directory on a remote archive via `mArchive->mkdirCmd(dir)`. If the archive has no mkdir command, the command string is empty and the work succeeds immediately. Retries `RETRY_A_LOT`.

### `GzipFileWork` (GzipFileWork.h/cpp)
**Inherits:** `RunCommandWork`

Compresses a local file using `gzip`. Supports a `keepExisting` mode that uses `gzip -c` and redirects to an output file. On reset, removes the `.gz` file.

### `GunzipFileWork` (GunzipFileWork.h/cpp)
**Inherits:** `RunCommandWork`

Decompresses a `.gz` file using `gzip -d`. Supports `keepExisting` mode. Defaults to `RETRY_NEVER`. On reset, removes the decompressed file.

---

## Composite Download Operations

### `GetAndUnzipRemoteFileWork` (GetAndUnzipRemoteFileWork.h/cpp)
**Inherits:** `Work`

Two-phase work: downloads a gzipped file from a history archive then gunzips it locally. Orchestrates `GetRemoteFileWork` → file validation (rename `.gz.tmp` to `.gz`) → `GunzipFileWork`.

**Key functions:**
- `doWork()`: Three-state machine: (1) spawn `GetRemoteFileWork`, (2) on download success, validate file and spawn `GunzipFileWork`, (3) check gunzip result and verify `.nogz` file exists.
- `validateFile()`: Renames `.gz.tmp` → `.gz`, checking existence at each step.
- `doReset()`: Removes all local file variants (`.nogz`, `.gz`, `.gz.tmp`).
- `onSuccess()`: Notifies `LedgerApplyManager::fileDownloaded()`.
- `onFailureRaise()`: Logs potential archive corruption.
- `getArchive()`: Returns the archive used if download succeeded.

**Key data:**
- `mFt` (FileTransferInfo): Describes the file being transferred (type, paths, checkpoint).
- `mArchive`: Optional fixed archive.
- `mGetRemoteFileWork`, `mGunzipFileWork`: Child work references.

### `BatchDownloadWork` (BatchDownloadWork.h/cpp)
**Inherits:** `BatchWork`

Downloads a range of checkpoint files of a given `FileType` (ledger headers, transactions, results, SCP messages). Iterates over a `CheckpointRange`, yielding one `GetAndUnzipRemoteFileWork` per checkpoint. `BatchWork` manages parallelism.

**Key functions:**
- `yieldMoreWork()`: Creates a `GetAndUnzipRemoteFileWork` for the next checkpoint in range, advances `mNext`.
- `hasNext()`: Returns true if `mNext < mRange.limit()`.
- `resetIter()`: Resets `mNext` to `mRange.mFirst`.

**Key data:**
- `mRange` (CheckpointRange): The range of checkpoints to download.
- `mNext` (uint32_t): Next checkpoint to yield.
- `mFileType` (FileType): Type of history files to download.
- `mDownloadDir` (TmpDir ref): Local temp directory for downloads.

---

## Bucket Download & Verification

### `DownloadBucketsWork` (DownloadBucketsWork.h/cpp)
**Inherits:** `BatchWork`

Downloads, verifies, and adopts all bucket files needed for catchup. Handles both `LiveBucket` and `HotArchiveBucket` types via a templated inner `BucketState<BucketT>` struct. Each bucket goes through a three-step sequence: download → verify+index → adopt.

**Key functions:**
- `yieldMoreWork()`: For each bucket hash, creates a `WorkSequence` of: `GetAndUnzipRemoteFileWork` → `VerifyBucketWork<BucketT>` → `WorkWithCallback` (adopt). Iterates live buckets first, then hot archive buckets.
- `prepareWorkForBucketType<BucketT>()`: Template helper that creates the verify work and the adopt callback, managing index storage and mutex locking.
- `onSuccessCb<BucketT>()`: Static callback that extracts the verified index, calls `BucketManager::adoptFileAsBucket`, and stores the result in the output map.

**Key data:**
- `BucketState<BucketT>`: Inner template struct containing:
  - `buckets`: Reference to output map of hash→Bucket.
  - `hashes`: Vector of bucket hashes to download.
  - `nextIter`: Iterator tracking progress.
  - `indexMap`: Map of ID→index pointer, used for ownership transfer between verify and adopt steps.
  - `mutex`: Protects concurrent access to `buckets` and `indexMap`.
  - `indexId`: Monotonic counter for indexMap keys.
- `mLiveBucketsState`, `mHotBucketsState`: Separate state for each bucket type.

### `VerifyBucketWork<BucketT>` (VerifyBucketWork.h/cpp)
**Inherits:** `BasicWork` (template class)

Verifies a bucket file's SHA-256 hash and builds its index, running on a background thread. Template instantiated for `LiveBucket` and `HotArchiveBucket`.

**Key functions:**
- `onRun()`: If not done, calls `spawnVerifier()` and returns `WORK_WAITING`.
- `spawnVerifier()`: Checks bucket size against `MAX_HISTORY_ARCHIVE_BUCKET_SIZE`, then posts work to background thread. Background thread calls `createIndex<BucketT>()` (which also computes the hash via a `SHA256` hasher), then posts result back to main thread setting `mIndex`, `mEc`, `mDone`.
- `onFailureRaise()`: Calls `mOnFailure` callback if set.

**Key data:**
- `mBucketFile` (string): Path to the bucket file.
- `mHash` (uint256): Expected hash.
- `mIndex` (shared_ptr ref): Output index pointer, written by the background verifier.
- `mOnFailure` (OnFailureCallback): Called on verification failure for logging.
- `mDone` (bool), `mEc` (error_code): Completion status.

---

## Transaction Result Verification

### `VerifyTxResultsWork` (VerifyTxResultsWork.h/cpp)
**Inherits:** `BasicWork`

Verifies transaction results for a single checkpoint by comparing `txSetResultHash` in ledger headers against computed SHA-256 hashes of transaction result sets. Runs verification on a background thread.

**Key functions:**
- `onRun()`: Posts `verifyTxResultsOfCheckpoint()` to background thread. On completion, posts result back to main thread.
- `verifyTxResultsOfCheckpoint()`: Opens ledger header and result XDR files, iterates through all headers in the checkpoint, loads corresponding result sets, and verifies each hash matches.
- `getCurrentTxResultSet()`: Reads from the result XDR stream, validates ledger is within checkpoint range and monotonically increasing.

**Key data:**
- `mDownloadDir` (TmpDir ref): Directory containing downloaded files.
- `mCheckpoint` (uint32_t): The checkpoint being verified.
- `mHdrIn`, `mResIn` (XDRInputFileStream): Streams for header and result files.
- `mLastSeenLedger` (uint32_t): Tracks monotonic ordering of result entries.

### `DownloadVerifyTxResultsWork` (DownloadVerifyTxResultsWork.h/cpp)
**Inherits:** `BatchWork`

Batch work that downloads and verifies transaction results for a range of checkpoints. Each checkpoint yields a `WorkSequence` of `GetAndUnzipRemoteFileWork` (results) → `VerifyTxResultsWork`.

---

## History Archive State

### `GetHistoryArchiveStateWork` (GetHistoryArchiveStateWork.h/cpp)
**Inherits:** `Work`

Downloads and parses a `HistoryArchiveState` (HAS) JSON file from an archive. The HAS describes the current state of an archive including its latest ledger and bucket list references.

**Key functions:**
- `doWork()`: Spawns `GetRemoteFileWork` to download the HAS file; on success, calls `mState.load(mLocalFilename)` to parse the JSON.
- `getHistoryArchiveState()`: Accessor (only valid after `WORK_SUCCESS`).
- `getRemoteName()`: Returns either the well-known path (seq==0) or a ledger-specific path.
- `onSuccess()`: Optionally reports metrics via `LedgerApplyMananger::historyArchiveStatesDownloaded()`.

**Key data:**
- `mState` (HistoryArchiveState): Parsed result.
- `mSeq` (uint32_t): Target ledger sequence (0 = latest/well-known).
- `mArchive`: Archive to fetch from (null = random).
- `mLocalFilename` (string): Temp local file path (random hex name).

### `PutHistoryArchiveStateWork` (PutHistoryArchiveStateWork.h/cpp)
**Inherits:** `Work`

Serializes and uploads a `HistoryArchiveState` to an archive. Validates that the HAS contains valid buckets before publishing. Uploads to both the ledger-specific path and the well-known path (`/.well-known/stellar-history.json`).

**Key functions:**
- `doWork()`: Saves HAS to local file, then calls `spawnPublishWork()`.
- `spawnPublishWork()`: Creates two parallel `WorkSequence`s: one for the seq-specific path and one for the well-known path. Each sequence is `MakeRemoteDirWork` → `PutRemoteFileWork`.

---

## Publishing Pipeline

### `ResolveSnapshotWork` (ResolveSnapshotWork.h/cpp)
**Inherits:** `BasicWork`

Waits for a `StateSnapshot`'s bucket futures to resolve. Delays one ledger past the snapshot ledger (unless standalone) to guard against publishing divergent data.

**Key functions:**
- `onRun()`: Calls `prepareForPublish()` and `resolveAnyReadyFutures()` on the snapshot. If all futures are resolved and we're past the conservative delay, returns `WORK_SUCCESS`. Otherwise sets up a 1-second polling wait.

### `WriteSnapshotWork` (WriteSnapshotWork.h/cpp)
**Inherits:** `BasicWork`

Writes SCP messages from a `StateSnapshot` to local files. Runs on a background thread if DB connection pooling is available, otherwise on the main thread via `postOnMainThread`.

**Key functions:**
- `onRun()`: Posts a lambda that calls `mSnapshot->writeSCPMessages()`. On completion, posts back to main thread setting `mDone` and `mSuccess`.

### `PutSnapshotFilesWork` (PutSnapshotFilesWork.h/cpp)
**Inherits:** `Work`

Three-phase orchestrator for uploading a snapshot to all writable archives:
1. **Get archive states:** Spawns `GetHistoryArchiveStateWork` for each writable archive to learn what files they already have.
2. **Gzip files:** Compresses only the files that differ between the snapshot and each archive's current state (avoids redundant uploads). Uses `StateSnapshot::differingHASFiles()`.
3. **Upload:** For each archive, spawns a `WorkSequence` of `PutFilesWork` → `PutHistoryArchiveStateWork`.

**Key data:**
- `mGetStateWorks`: List of archive state download works.
- `mGzipFilesWorks`: List of gzip works for differing files.
- `mUploadSeqs`: List of upload work sequences.
- `mFilesToUpload`: Map of local path → `FileTransferInfo` (deduplicates across archives).

### `PutFilesWork` (PutFilesWork.h/cpp)
**Inherits:** `Work`

Uploads all differing files for a single archive. For each file from `mSnapshot->differingHASFiles(remoteState)`, creates a `WorkSequence` of `MakeRemoteDirWork` → `PutRemoteFileWork`.

### `PublishWork` (PublishWork.h/cpp)
**Inherits:** `WorkSequence`

Top-level publish work that wraps a sequence of publish steps. On success or failure, notifies `HistoryManager::historyPublished()` with the ledger number and bucket hashes. Stores `mOriginalBuckets` separately because the snapshot's bucket list may change during async execution.

---

## Verification & Integrity Checking

### `CheckSingleLedgerHeaderWork` (CheckSingleLedgerHeaderWork.h/cpp)
**Inherits:** `Work`

Offline self-check: downloads the checkpoint file containing a given `LedgerHeaderHistoryEntry`, scans it, and verifies the archive copy matches the expected local copy. Used by the offline self-check command.

**Key functions:**
- `doWork()`: Downloads checkpoint via `GetAndUnzipRemoteFileWork`, then synchronously scans the XDR file comparing each header against `mExpected`.

**Key data:**
- `mExpected` (LedgerHeaderHistoryEntry): The expected header to verify.
- `mArchive`: The archive to check against.
- `mCheckSuccess`, `mCheckFailed`: Medida metrics.

### `WriteVerifiedCheckpointHashesWork` (WriteVerifiedCheckpointHashesWork.h/cpp)
**Inherits:** `BatchWork`

Produces a JSON file of verified `[ledger_seq, hash]` pairs by downloading ledger header files and running `VerifyLedgerChainWork` on them in a chained fashion. Works backwards from a trusted `mRangeEnd` toward genesis (or a `fromLedger`/`latestTrustedHashPair` if specified).

**Key functions:**
- `yieldMoreWork()`: For each batch, creates a `WorkSequence` of `BatchDownloadWork` (ledger headers) → `ConditionalWork` wrapping `VerifyLedgerChainWork`. Each `VerifyLedgerChainWork` depends on the previous one's verified hash output via a `shared_future<LedgerNumHashPair>`.
- `startOutputFile()` / `endOutputFile()`: Manage the JSON output file lifecycle. If a `trustedHashFile` is provided, its content is appended to the output.
- `loadHashFromJsonOutput()` / `loadLatestHashPairFromJsonOutput()`: Static helpers to read back hashes from the JSON output.

**Key data:**
- `mRangeEnd` (LedgerNumHashPair): The trusted endpoint (highest ledger).
- `mRangeEndPromise` / `mRangeEndFuture`: Promise/future pair providing the trusted hash to the first link in the verification chain.
- `mCurrCheckpoint` (uint32_t): Current iteration point, decreasing toward genesis.
- `mPrevVerifyWork`: Previous `VerifyLedgerChainWork`, whose output future feeds the next batch.
- `mNestedBatchSize`: Controls inner parallelism (default 64 checkpoints per batch).
- `mTmpDirs`: Vector of (WorkSequence, TmpDir) pairs; TmpDirs are cleaned up as sequences complete.
- `mOutputFile`: Shared output stream written by `VerifyLedgerChainWork` instances.
- `mTrustedHashPath`, `mLatestTrustedHashPair`, `mFromLedger`: Optional parameters for incremental verification.

---

## SCP / Quorum Set Fetching

### `FetchRecentQsetsWork` (FetchRecentQsetsWork.h/cpp)
**Inherits:** `Work`

Three-phase work for downloading and scanning recent SCP messages to discover active quorum sets:
1. Fetches the latest archive state via `GetHistoryArchiveStateWork`.
2. Downloads SCP message files for the last ~100 checkpoints (~9 hours) via `BatchDownloadWork`.
3. Scans downloaded XDR files to extract `SCPHistoryEntry` records.

---

## Key Data Flows

### Publish Flow
```
ResolveSnapshotWork (wait for bucket futures)
  → WriteSnapshotWork (write SCP messages to local files)
    → PutSnapshotFilesWork
      → GetHistoryArchiveStateWork (per archive, get current state)
      → GzipFileWork (gzip only differing files)
      → PutFilesWork (per archive: MakeRemoteDirWork → PutRemoteFileWork per file)
      → PutHistoryArchiveStateWork (upload HAS JSON to seq path + well-known path)
```
All wrapped in `PublishWork` (a `WorkSequence`) which notifies `HistoryManager` on completion.

### Download/Catchup Flow
```
BatchDownloadWork (download checkpoint files of a given type: ledgers, txs, results, SCP)
  → GetAndUnzipRemoteFileWork (per checkpoint)
    → GetRemoteFileWork (download .gz)
    → GunzipFileWork (decompress)

DownloadBucketsWork (download+verify+adopt all buckets)
  → per bucket: GetAndUnzipRemoteFileWork → VerifyBucketWork → adopt callback

DownloadVerifyTxResultsWork (download+verify tx results)
  → per checkpoint: GetAndUnzipRemoteFileWork → VerifyTxResultsWork
```

### Verified Checkpoint Hash Chain
```
WriteVerifiedCheckpointHashesWork (iterates backwards from trusted endpoint)
  → per batch: BatchDownloadWork (ledger headers)
    → ConditionalWork(predicate: prev batch succeeded)
      → VerifyLedgerChainWork (verifies hash chain, writes to shared output file)
         (chained via shared_future<LedgerNumHashPair> from previous batch)
```

---

## Threading Model

- **Main thread**: All `Work` state machine transitions, scheduling, and `doWork()`/`onRun()` calls.
- **Background threads** (via `postOnBackgroundThread`):
  - `VerifyBucketWork::spawnVerifier()`: SHA-256 hashing and index creation.
  - `VerifyTxResultsWork::onRun()`: Transaction result verification.
  - `WriteSnapshotWork::onRun()`: SCP message writing (if DB pooling available).
- **External processes** (via `ProcessManager::runProcess`): All `RunCommandWork` subclasses (gzip, gunzip, get/put remote files, mkdir). These spawn shell commands and use async `ProcessExitEvent` callbacks.
- **Synchronization**: `DownloadBucketsWork::BucketState` uses `std::mutex` to protect `buckets` and `indexMap` maps accessed from both main and background threads. Background workers always post results back to main thread via `postOnMainThread` before modifying `BasicWork` state.

---

## Ownership & Lifetime

- `Work` objects form a tree: parent works own child works via `addWork<T>()`. The work scheduler drives the tree.
- `StateSnapshot` is shared across the publish pipeline via `shared_ptr`.
- `TmpDir` objects own temporary directories; their destructors clean up files. `WriteVerifiedCheckpointHashesWork` explicitly manages TmpDir lifetime per batch.
- `HistoryArchive` is shared via `shared_ptr` and may be null (meaning "pick randomly").
- `FileTransferInfo` is a value type describing file paths and types; not heap-allocated.
- `BatchWork` (parent class) manages the pool of active child works and controls parallelism.
