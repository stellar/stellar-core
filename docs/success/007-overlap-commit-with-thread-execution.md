# Experiment 007: Overlap Per-Thread Commit with Parallel Execution

## Date
2026-02-20

## Hypothesis
The serial `commitChangesFromThreads` phase (47ms/stage) runs entirely after
all 4 worker threads complete. Two sub-operations can be overlapped with thread
execution:

1. `getReadWriteKeysForStage` (19ms) — only reads TX footprints, independent
   of thread results. Can be computed on the main thread while workers execute.
2. Per-thread `commitChangesFromThread` (6.4ms each) — can be done as each
   thread finishes via `future.get()`, overlapping commit of early-finishing
   threads with still-running threads.

Expected savings: ~30-40ms per stage by fully overlapping the commit work with
thread execution.

## Change Summary
Restructured `applySorobanStageClustersInParallel` to combine thread execution
and per-thread commit into a single function:

1. Deactivate global scope → construct thread states → launch threads
2. Reactivate global scope (worker threads don't access it during execution)
3. Pre-compute `readWriteSet` on main thread while workers run
4. As each thread finishes (`future.get()`), immediately commit its changes

This eliminates the separate `commitChangesFromThreads` call that previously
ran serially after all threads completed.

Key insight: the LedgerEntryScope deactivation prevents accidental reads of
stale global state, but worker threads never access the global scope during
execution (they have thread-local state). So the global scope can be safely
reactivated for commit work while threads are still running.

## Results

### TPS
- Baseline: 9,408 TPS
- Post-change: 10,688 TPS
- Delta: **+13.6% / +1,280 TPS**

### Tracy Analysis

| Zone | Old Mean (ms) | New Mean (ms) | Notes |
|------|--------------|--------------|-------|
| `applySorobanStage` | 811.9 | 810.4 | Same total, but 13.6% more TXs |
| `applySorobanStageClustersInParallel` | 754.7 | 807.9 | Now includes commit work |
| `commitChangesFromThreads` | 47.1 | GONE | Eliminated — merged into parallel |
| `getReadWriteKeysForStage` | 19.2 | 23.6 | Now overlapped with thread execution |
| `commitChangesFromThread` ×4 | 25.4 | 26.3 | Now overlapped with thread execution |
| `commitChangesToLedgerTxn` | 50.6 | 48.0 | Unchanged |
| `applySorobanStages` | 991.3 | 990.4 | Same total — processing 13.6% more TXs |

The per-stage total time is essentially unchanged (~810ms), but now processes
13.6% more transactions per stage. The 47ms of serial commit overhead is fully
absorbed into the thread execution phase.

## Files Changed
- `src/ledger/LedgerManagerImpl.h` — Changed `applySorobanStageClustersInParallel` signature: returns void, takes non-const globalState
- `src/ledger/LedgerManagerImpl.cpp` — Restructured to combine parallel execution and per-thread commit; simplified `applySorobanStage`
- `src/transactions/ParallelApplyUtils.h` — Made `commitChangesFromThread` public; declared `getReadWriteKeysForStage` in header
- `src/transactions/ParallelApplyUtils.cpp` — Moved `getReadWriteKeysForStage` from anonymous namespace to `stellar` namespace; removed `commitChangesFromThreads`

## Commit
