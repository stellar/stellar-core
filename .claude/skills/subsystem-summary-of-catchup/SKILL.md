---
name: subsystem-summary-of-catchup
description: "read this skill for a token-efficient summary of the catchup subsystem"
---

# Catchup Subsystem — Technical Summary

The catchup subsystem in stellar-core is responsible for synchronizing a node's local ledger state with the rest of the network when it falls behind. It downloads historical data (ledger headers, transactions, and bucket snapshots) from history archives, verifies integrity, and applies the data to bring the node up to date.

All files reside in `src/catchup/`.

---

## Key Classes and Data Structures

### CatchupConfiguration
**File:** `CatchupConfiguration.h/.cpp`

Immutable configuration describing a catchup request. Parameterized by:
- `toLedger` — destination ledger number (or `CURRENT = 0` to resolve at runtime from the archive).
- `count` — number of ledgers to replay before the destination. `0` = minimal (buckets only), `UINT32_MAX` = complete history.
- `Mode` — one of `OFFLINE_BASIC`, `OFFLINE_COMPLETE`, or `ONLINE`.

Key methods:
- `resolve(uint32_t remoteCheckpoint)` — substitutes `CURRENT` with an actual checkpoint ledger number.
- `offline()` / `online()` — predicates for mode.

Helper free functions `parseLedger()` and `parseLedgerCount()` parse CLI strings.

### CatchupRange
**File:** `CatchupRange.h/.cpp`

Computed from `CatchupConfiguration` + the current LCL + `HistoryManager`. Decides **what** the catchup must do:

| Field | Meaning |
|---|---|
| `mApplyBuckets` | Whether a bucket-apply phase is needed. |
| `mApplyBucketsAtLedger` | Checkpoint ledger at which to apply buckets (0 if !mApplyBuckets). |
| `mReplayRange` (LedgerRange) | Half-open range of ledgers to replay after bucket-apply. |

Five logical cases based on LCL position, requested count, and checkpoint boundaries (see comments in header). Invariants enforced by `checkInvariants()`.

Key accessors: `applyBuckets()`, `replayLedgers()`, `getBucketApplyLedger()`, `getReplayRange()`, `getFullRangeIncludingBucketApply()`.

### LedgerApplyManager / LedgerApplyManagerImpl
**Files:** `LedgerApplyManager.h`, `LedgerApplyManagerImpl.h/.cpp`

Abstract interface + concrete implementation. This is the **top-level coordinator** between the consensus layer (Herder) and the catchup/apply machinery. Owned by `Application`.

#### Key data members (Impl):
- `mCatchupWork` — `shared_ptr<CatchupWork>`, the running catchup work item (null when not catching up).
- `mSyncingLedgers` — `map<uint32_t, LedgerCloseData>`, buffer of ledgers received from the network that cannot be applied yet. Has strict invariants: either empty, starts at LCL+1, or contains at most 65 ledgers within a checkpoint boundary.
- `mLastQueuedToApply` — tracks the highest ledger sequence queued for application.
- `mLargestLedgerSeqHeard` — the highest ledger seq ever received.
- `mMetrics` (`CatchupMetrics`) — counters for archive states downloaded, checkpoints, ledgers verified, buckets downloaded/applied, tx sets downloaded/applied.
- `mCatchupFatalFailure` — set when catchup fails unrecoverably (e.g., incompatible core version).
- `MAX_EXTERNALIZE_LEDGER_APPLY_DRIFT = 12` — maximum ledger drift allowed before entering catchup in parallel-close mode.

#### Key methods:
- **`processLedger(LedgerCloseData, isLatestSlot)`** — main entry point called by Herder/LedgerManager when a new consensus ledger arrives. Logic:
  1. If catchup is done, resets `mCatchupWork`.
  2. If ledger is old (≤ mLastQueuedToApply), skip.
  3. If ledger is the next sequential one and no catchup running → `tryApplySyncingLedgers()`.
  4. Otherwise buffers the ledger, trims the buffer, and decides whether to `startOnlineCatchup()`.
- **`startCatchup(CatchupConfiguration, archive)`** — schedules a `CatchupWork` on the `WorkScheduler`.
- **`startOnlineCatchup()`** — constructs a `CatchupConfiguration` targeting `firstBuffered - 1` in ONLINE mode.
- **`trimSyncingLedgers()`** — garbage-collects old entries from `mSyncingLedgers`, keeping at most one checkpoint's worth plus one.
- **`tryApplySyncingLedgers()`** — iterates sequential ledgers in `mSyncingLedgers` and applies them via `LedgerManager::applyLedger()`. In parallel-close mode, posts work to the ledger-close thread.
- **`maybeGetNextBufferedLedgerToApply()`** — returns the next buffered ledger (LCL+1) if available; used by `ApplyBufferedLedgersWork`.

### CatchupWork
**File:** `CatchupWork.h/.cpp`

The central **Work** subclass orchestrating all catchup steps. Extends `Work` (composite work pattern).

#### Key data members:
- `mLocalState` (HistoryArchiveState) — local BucketList state at catchup start.
- `mDownloadDir` (unique_ptr<TmpDir>) — temporary directory for downloaded files.
- `mLiveBuckets`, `mHotBuckets` — maps from hash → downloaded Bucket objects.
- `mCatchupConfiguration` — the resolved configuration.
- `mGetHistoryArchiveStateWork`, `mGetBucketStateWork` — work to fetch HAS from archive.
- `mDownloadVerifyLedgersSeq` — work sequence for downloading + verifying ledger headers.
- `mVerifyLedgers` (VerifyLedgerChainWork) — verifies ledger chain integrity.
- `mBucketVerifyApplySeq` — work sequence for downloading, verifying, and applying buckets.
- `mTransactionsVerifyApplySeq` (DownloadApplyTxsWork) — work for downloading and applying transactions.
- `mApplyBufferedLedgersWork` — applies buffered network ledgers after catchup replay.
- `mCatchupSeq` — final composite work sequence.
- `mVerifiedLedgerRangeStart` (LedgerHeaderHistoryEntry) — the verified ledger at the start of the catchup range (used for bucket-apply).
- `mFatalFailureFuture` — shared_future indicating unrecoverable failure.

#### Key control flow (`runCatchupStep()` / `doWork()`):
1. **Get HAS** — `getAndMaybeSetHistoryArchiveState()` fetches the remote history archive state, validates network passphrase, checks that target > LCL.
2. **Resolve CatchupRange** — from config + HAS + LCL.
3. **Get bucket HAS** — `getAndMaybeSetBucketHistoryArchiveState()` if bucket-apply is needed and the bucket HAS differs from the main HAS.
4. **Download & verify ledger chain** — `downloadVerifyLedgerChain()` spawns `BatchDownloadWork` + `VerifyLedgerChainWork` in a `WorkSequence`.
5. **Build catchup sequence** — after ledger verification succeeds:
   - If `applyBuckets()`: `downloadApplyBuckets()` → `DownloadBucketsWork` + `ApplyBucketsWork`.
   - If `replayLedgers()`: `downloadApplyTransactions()` → `DownloadApplyTxsWork`.
   - A Herder consistency check work is prepended.
6. **Bucket-apply completion** — calls `LedgerManager::setLastClosedLedger()` with the verified state, clears rebuild flags.
7. **Apply buffered ledgers** — after the main catchup sequence succeeds, `ApplyBufferedLedgersWork` drains `mSyncingLedgers`.

Constants: `PUBLISH_QUEUE_UNBLOCK_APPLICATION = 8`, `PUBLISH_QUEUE_MAX_SIZE = 16` — flow-control the publish queue during catchup.

### VerifyLedgerChainWork
**File:** `VerifyLedgerChainWork.h/.cpp`

`BasicWork` subclass that verifies a range of downloaded ledger header files. Processes checkpoints from **highest to lowest**, linking each checkpoint's hash chain to the next.

#### Key data members:
- `mDownloadDir`, `mRange`, `mCurrCheckpoint` — the files to verify and current position.
- `mLastClosed` (LedgerNumHashPair) — local LCL for consistency checks.
- `mTrustedMaxLedger` (shared_future<LedgerNumHashPair>) — trusted hash from SCP consensus for the range end.
- `mVerifiedAhead` (LedgerNumHashPair) — hash-link propagation between checkpoint verifications.
- `mVerifiedMinLedgerPrev` (promise) — outgoing: the hash just before the verified range, so bucket-apply can validate.
- `mMaxVerifiedLedgerOfMinCheckpoint` — the max ledger of the lowest checkpoint; used by CatchupWork as `mVerifiedLedgerRangeStart`.
- `mFatalFailurePromise` — set when a mismatch against trusted hash is detected.
- `mChainDisagreesWithLocalState` — records local-state disagreements (e.g., bad LCL hash, incompatible version).

#### Key method — `verifyHistoryOfSingleCheckpoint()`:
- Opens the checkpoint ledger header file.
- Iterates entries, verifying each ledger header hash and link to the previous.
- At the range end, verifies against `mTrustedMaxLedger`.
- At each checkpoint boundary, checks hash-chain linkage with `mVerifiedAhead`.
- On the lowest checkpoint, writes hash-link to `mVerifiedMinLedgerPrev` and records `mMaxVerifiedLedgerOfMinCheckpoint`.
- Checks local state (LCL hash, protocol version) and records disagreements.

#### `onRun()`:
Calls `verifyHistoryOfSingleCheckpoint()` once per crank. On success, decrements `mCurrCheckpoint` and returns `WORK_RUNNING` until all checkpoints are verified. Maps various error statuses to `WORK_FAILURE` with appropriate log messages.

### DownloadApplyTxsWork
**File:** `DownloadApplyTxsWork.h/.cpp`

`BatchWork` subclass that iterates over checkpoints in a replay range, yielding a work sequence per checkpoint: download → unzip → apply.

#### Key data members:
- `mRange` (LedgerRange) — the half-open replay range.
- `mDownloadDir` — shared temp directory.
- `mLastApplied` (LedgerHeaderHistoryEntry&) — reference to the last applied header (updated on success).
- `mCheckpointToQueue` — next checkpoint to schedule.
- `mLastYieldedWork` — the previous checkpoint's work, used for sequencing.
- `mWaitForPublish` — if true, gates application on publish queue size.

#### `yieldMoreWork()`:
For each checkpoint:
1. Creates `GetAndUnzipRemoteFileWork` for the transaction file.
2. Creates `ApplyCheckpointWork` for the ledger range within that checkpoint.
3. Wraps application in a `ConditionalWork` that:
   - Waits for the previous checkpoint's work to finish.
   - Optionally waits for the publish queue to drain below `PUBLISH_QUEUE_MAX_SIZE`.
   - Optionally waits for BucketList merges.
4. Appends cleanup work to delete temporary files.
5. Returns the whole sequence as a `WorkSequence`.

### ApplyCheckpointWork
**File:** `ApplyCheckpointWork.h/.cpp`

`BasicWork` subclass that applies transactions from a single checkpoint (at most one checkpoint worth of ledgers).

#### Key data members:
- `mDownloadDir` — temp dir with ledger + tx files.
- `mLedgerRange` — the aligned ledger range to apply.
- `mCheckpoint` — the checkpoint number.
- `mHdrIn`, `mTxIn` — XDR input streams for ledger headers and transactions.
- `mConditionalWork` — wraps `ApplyLedgerWork` in a conditional that waits for BucketList merge futures to resolve.

#### Key control flow (`onRun()`):
1. If a conditional work is active, cranks it. On success, verifies the resulting LCL hash matches the expected header hash.
2. Checks if done (all ledgers in range applied).
3. Opens input files if needed.
4. Calls `getNextLedgerCloseData()` which reads the next header from file, performs knitting checks (skip old, verify LCL hash continuity, verify tx set hash), and constructs a `LedgerCloseData`.
5. Creates `ApplyLedgerWork` wrapped in a `ConditionalWork` that waits for BucketList merge futures.

### ApplyLedgerWork
**File:** `ApplyLedgerWork.h/.cpp`

Minimal `BasicWork` subclass. `onRun()` calls `LedgerManager::applyLedger(lcd, false)` to close a single ledger. No retry.

### ApplyBucketsWork
**File:** `ApplyBucketsWork.h/.cpp`

`Work` subclass that applies bucket snapshot state to the database.

#### Key data members:
- `mBuckets` — map of hash → LiveBucket (downloaded buckets).
- `mApplyState` (HistoryArchiveState) — the archive state to apply.
- `mBucketsToApply` — ordered vector of buckets (L0 curr, L0 snap, L1 curr, ...).
- `mBucketApplicator` — the active `BucketApplicator` instance.
- `mSeenKeys`, `mSeenKeysBeforeApply` — deduplication sets to ensure only the newest version of each entry is written.
- `mIndexBucketsWork` — child work to index bucket files (runs first).
- `mAssumeStateWork` — child work to assume BucketList state (runs after all buckets applied).

#### Key control flow (`doWork()`):
1. **Index buckets** — spawns `IndexBucketsWork<LiveBucket>` on first call.
2. **Apply buckets** — iterates through `mBucketsToApply` in order, using `BucketApplicator` to incrementally write entries to the database. Entries already in `mSeenKeys` are skipped (ensures newest-version-wins). After each bucket, runs invariant checks.
3. **Assume state** — spawns `AssumeStateWork` which indexes both live and hot archive buckets, then calls `BucketManager::assumeState()` to set the BucketList to the target state and restart merges.

### AssumeStateWork
**File:** `AssumeStateWork.h/.cpp`

`Work` subclass spawned at the end of `ApplyBucketsWork`. Holds strong references to all buckets in the HAS (including future buckets from pending merges) to prevent garbage collection during indexing.

#### `doWork()`:
1. Spawns `IndexBucketsWork<LiveBucket>` and `IndexBucketsWork<HotArchiveBucket>`.
2. Spawns a callback work that calls `BucketManager::assumeState()` and `InvariantManager::checkAfterAssumeState()`.
3. Returns `checkChildrenStatus()`.

### IndexBucketsWork<BucketT>
**File:** `IndexBucketsWork.h/.cpp`

Template `Work` subclass that indexes bucket files in parallel. For each non-empty, non-indexed bucket, spawns an `IndexWork` child.

#### IndexWork (inner class):
- Posts indexing to a background thread via `postOnBackgroundThread`.
- Tries to load a persisted index file first; if corrupt or outdated, creates a fresh index via `createIndex<BucketT>()`.
- On completion, posts result back to main thread and calls `BucketManager::maybeSetIndex()`.

### ApplyBufferedLedgersWork
**File:** `ApplyBufferedLedgersWork.h/.cpp`

`BasicWork` subclass used at the end of catchup to drain `mSyncingLedgers`. On each `onRun()`:
1. Checks if previous `ConditionalWork` is done.
2. Asks `LedgerApplyManager::maybeGetNextBufferedLedgerToApply()` for the next ledger.
3. Wraps `ApplyLedgerWork` in a `ConditionalWork` that waits for BucketList merge futures.
4. Returns `WORK_SUCCESS` when no more buffered ledgers available.

### ReplayDebugMetaWork
**File:** `ReplayDebugMetaWork.h/.cpp`

`Work` subclass for offline replay of debug meta files (used in diagnostic scenarios). Iterates sorted debug meta files, optionally gunzips them, and spawns `ApplyLedgersFromMetaWork` (inner helper class) to read `LedgerCloseMeta` entries and apply them via `ApplyLedgerWork`. Can also apply a final `StoredDebugTransactionSet` for the latest ledger.

---

## Key Data Flows

### Online Catchup Flow
```
Herder (consensus)
  │
  ▼
LedgerApplyManagerImpl::processLedger()
  │
  ├─ If sequential with LCL → tryApplySyncingLedgers() → LedgerManager::applyLedger()
  │
  └─ If behind → buffer in mSyncingLedgers
       │
       └─ When checkpoint boundary reached → startOnlineCatchup()
            │
            ▼
          CatchupWork (scheduled on WorkScheduler)
            │
            ├─ 1. GetHistoryArchiveStateWork → fetch remote HAS
            ├─ 2. Compute CatchupRange
            ├─ 3. downloadVerifyLedgerChain()
            │     ├─ BatchDownloadWork (ledger header files)
            │     └─ VerifyLedgerChainWork (hash-chain verification, highest→lowest)
            ├─ 4a. downloadApplyBuckets() [if applyBuckets()]
            │      ├─ DownloadBucketsWork
            │      ├─ verify HAS
            │      └─ ApplyBucketsWork
            │           ├─ IndexBucketsWork
            │           ├─ BucketApplicator (per bucket, level by level)
            │           └─ AssumeStateWork
            ├─ 4b. downloadApplyTransactions() [if replayLedgers()]
            │      └─ DownloadApplyTxsWork (per checkpoint)
            │           ├─ GetAndUnzipRemoteFileWork
            │           └─ ApplyCheckpointWork
            │                └─ ApplyLedgerWork (per ledger)
            └─ 5. ApplyBufferedLedgersWork → drain mSyncingLedgers
```

### Offline Catchup Flow
Same as online but triggered by `startCatchup()` directly (not by buffered ledgers), mode is `OFFLINE_BASIC` or `OFFLINE_COMPLETE`, no `ApplyBufferedLedgersWork`, and in `OFFLINE_COMPLETE` mode, `DownloadVerifyTxResultsWork` is also run for full validation.

---

## Threading Model

- All `LedgerApplyManagerImpl` methods assert `threadIsMain()` — the catchup coordinator runs entirely on the main thread.
- The `Work` / `BasicWork` framework is cranked on the main thread's event loop.
- `IndexBucketsWork::IndexWork` posts indexing tasks to a background thread pool via `postOnBackgroundThread()`, and posts results back to the main thread via `postOnMainThread()`.
- In parallel-close mode (`parallelLedgerClose()`), `tryApplySyncingLedgers()` posts `applyLedger` calls to the ledger-close thread.
- `ApplyCheckpointWork` and `ApplyBufferedLedgersWork` use `ConditionalWork` to poll for BucketList merge future resolution before applying ledgers, preventing application while background merges are pending.
- `VerifyLedgerChainWork` uses `std::promise` / `std::shared_future` for inter-work communication: the trusted max-ledger hash is passed in via `shared_future`, and the verified min-ledger-prev hash is passed out via `promise`.

---

## Ownership Relationships

```
Application
 └─ LedgerApplyManagerImpl (unique_ptr, via LedgerApplyManager::create)
      └─ mCatchupWork: shared_ptr<CatchupWork> (owned while catchup active)
           ├─ mDownloadDir: unique_ptr<TmpDir>
           ├─ mLiveBuckets / mHotBuckets: map<string, shared_ptr<Bucket>>
           ├─ mGetHistoryArchiveStateWork: shared_ptr
           ├─ mDownloadVerifyLedgersSeq: shared_ptr<WorkSequence>
           │    └─ mVerifyLedgers: shared_ptr<VerifyLedgerChainWork>
           ├─ mBucketVerifyApplySeq: shared_ptr<WorkSequence>
           │    └─ ApplyBucketsWork
           │         ├─ mIndexBucketsWork: shared_ptr<IndexBucketsWork>
           │         │    └─ IndexWork children (per bucket, background thread)
           │         ├─ mBucketApplicator: unique_ptr<BucketApplicator>
           │         └─ mAssumeStateWork: shared_ptr<AssumeStateWork>
           ├─ mTransactionsVerifyApplySeq: shared_ptr<DownloadApplyTxsWork>
           │    └─ per-checkpoint WorkSequence children
           │         ├─ GetAndUnzipRemoteFileWork
           │         └─ ApplyCheckpointWork
           │              └─ mConditionalWork → ApplyLedgerWork
           ├─ mApplyBufferedLedgersWork: shared_ptr<ApplyBufferedLedgersWork>
           └─ mCatchupSeq: shared_ptr<WorkSequence> (final composite)
```

`LedgerApplyManagerImpl` also owns `mSyncingLedgers` (the ledger buffer) independently of `CatchupWork`.

---

## Key Invariants and Error Handling

- `CatchupRange::checkInvariants()` ensures at least one of bucket-apply or replay is active, and validates sequencing between them.
- Hash-chain verification in `VerifyLedgerChainWork` is done backwards (highest checkpoint first) to propagate trust from the SCP-consensus hash downward.
- If `VerifyLedgerChainWork` detects a mismatch with a trusted SCP hash, it sets `mFatalFailurePromise` to true, causing `CatchupWork::fatalFailure()` to return true and `LedgerApplyManagerImpl` to set `mCatchupFatalFailure`, permanently blocking further catchup attempts.
- `ApplyCheckpointWork` validates that the resulting LCL hash matches the expected ledger header after each ledger application.
- Publish queue flow control in `DownloadApplyTxsWork` prevents the publish queue from growing beyond `PUBLISH_QUEUE_MAX_SIZE` by gating `ApplyCheckpointWork` behind a `ConditionalWork`.
- BucketList merge futures are awaited (via `ConditionalWork`) before applying any ledger, both during checkpoint replay and buffered ledger application.
