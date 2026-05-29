# Experiment 057: Reserve parallel apply container capacity

## Date
2026-02-24

## Hypothesis
`ParallelApplyEntryMap` (unordered_map) containers in the parallel apply path
grow incrementally via insert, causing log2(N) rehashes as they accumulate
entries. With ~64K entries across global/thread maps, this means ~16 rehash
operations per map, each rehashing all existing entries. By pre-computing the
expected entry count from footprint sizes and calling `reserve()` upfront, we
eliminate all rehashing overhead.

Experiment 014a attempted this previously but was blocked by sandbox test
infrastructure issues and was never benchmarked. The test infrastructure has
since been fixed (experiments 055-056 passed tests).

## Change Summary
Three `reserve()` additions to `ParallelApplyUtils.cpp`:

1. **`getReadWriteKeysForStage`**: Reserve `res` unordered_set based on
   estimated RW key count (each RW key may have a TTL key, so × 2). Note:
   this function runs concurrently with parallel threads, so its impact on
   TPS is limited.

2. **`GlobalParallelApplyLedgerState` constructor**: Reserve `mGlobalEntryMap`
   based on total footprint sizes across all stages (RW × 2 + RO × 2 + 1
   per TX for classic source account).

3. **`collectClusterFootprintEntriesFromGlobal`**: Reserve `mThreadEntryMap`
   based on cluster footprint sizes (RW × 2 + RO × 2 per TX in cluster).

## Results

### TPS
- Baseline: 18,368 TPS
- Post-change: 18,944 TPS
- Delta: +576 TPS (+3.1%)

### Tracy Analysis
- `applyLedger` avg: 987ms (baseline: 1,005ms) — **-18ms (-1.8%)**
- `commitChangesFromThread` self-time: 128ms (baseline: 173ms) — **-45ms (-26%)**
- `commitChangesToLedgerTxn` self-time: 120ms (baseline: 164ms) — **-44ms (-27%)**
- `getReadWriteKeysForStage` self-time: 138ms (baseline: 152ms) — **-14ms (-9%)**
- `upsertEntry` cumulative self-time: 425ms (baseline: 446ms) — -21ms (-5%)
- `updateState` self-time: 299ms (baseline: 309ms) — -10ms (noise)
- `addLiveBatch` avg: ~112ms (baseline: ~111ms) — flat

## Why It Worked
The commit-related functions (`commitChangesFromThread`, `commitChangesToLedgerTxn`)
showed the largest improvements (-26% to -27%) because they merge thread-local
maps into the global map. Without `reserve()`, each merge triggers progressive
rehashing as the destination map grows. With `reserve()`, the destination map
is pre-sized to accommodate all entries, so inserts never trigger rehash.

The thread-local map reserve in `collectClusterFootprintEntriesFromGlobal`
benefits both the per-TX `upsertEntry` calls (entries insert without rehash)
and the subsequent `commitChangesFromThread` call (the source map is already
properly sized).

## Files Changed
- `src/transactions/ParallelApplyUtils.cpp` — Added reserve() calls to
  getReadWriteKeysForStage, GlobalParallelApplyLedgerState constructor,
  and ThreadParallelApplyLedgerState::collectClusterFootprintEntriesFromGlobal

## Commit
(pending)
