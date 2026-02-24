# Experiment 027: Per-Entry ReadWrite Existence Tracking

## Date
2026-02-21

## Hypothesis
The `mAllReadWriteEntriesExist` flag in `TxParallelApplyLedgerState` is
all-or-nothing: if ANY readWrite entry doesn't exist, ALL entries take the
slow `upsertEntry` path (which calls `getLiveEntryOpt` to check existence).
For SAC transfers, destination balances are newly created so the flag is
always false, but source balances DO exist. Tracking per-entry existence
with a set and using `upsertEntryKnownExisting` for known entries should
save ~1.1µs per TX (skipping getLiveEntryOpt for 1 of 3 entries).

## Change Summary
- Added `UnorderedSet<LedgerKey> mLoadedReadWriteKeys` member to
  `InvokeHostFunctionApplyHelper`
- In `addReads`: when loading readWrite entries, inserted their keys into
  the set
- In `recordStorageChanges`: checked the set to call
  `upsertEntryKnownExisting` (fast path) for entries known to exist

## Results

### TPS
- Baseline (exp-026): 14,784 TPS
- Post-change: 14,720 TPS (within noise, -0.4%)

### Tracy Analysis
- upsertEntry: 128K calls (2/TX) at 2,238ns avg
- upsertEntryKnownExisting: 64K calls (1/TX) at 561ns avg
- Net savings: ~0.5µs/TX after hash set overhead (gross ~1.1µs)

## Why It Failed
The optimization saved only ~0.5µs per TX net (after hash set insert/lookup
overhead). This is <0.5% of the ~122µs per-TX cost in the parallel phase,
well within benchmark noise. The hash set operations for `UnorderedSet<LedgerKey>`
were expensive enough to eat half the savings from skipping getLiveEntryOpt.

## Key Discovery
Debug output during the experiment revealed that `mAllReadWriteEntriesExist`
is ALWAYS false for SAC transfer benchmarks because destination trust line
balances don't exist yet (rwLoaded=1, rwFootprintSize=2). This makes the
all-or-nothing flag useless for this workload.

## Files Changed
- `src/transactions/InvokeHostFunctionOpFrame.cpp` — added per-entry tracking
  (reverted)
