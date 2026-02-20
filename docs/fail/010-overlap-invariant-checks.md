# Experiment 010: Overlap Invariant Checking with Parallel Execution

## Date
2026-02-20

## Hypothesis
Moving `checkAllTxBundleInvariants` (invariant checks + `maybeSetRefundableFeeMeta`)
into the commit loop inside `applySorobanStageClustersInParallel` would overlap
invariant checking with still-running threads, reducing total apply time.

## Change Summary
Inlined `checkAllTxBundleInvariants` into the commit loop so each cluster's
invariant checking happens immediately after its thread results are committed
to the global map. Removed the standalone `checkAllTxBundleInvariants` function.

## Results

### TPS
- Baseline: 10,688 TPS
- Post-change: 10,688 TPS
- Delta: 0% (no measurable improvement)

### Tracy Analysis
The invariant checking + `maybeSetRefundableFeeMeta` consumes negligible time
compared to overall apply time. The overlap with thread execution saved
essentially zero wall-clock time.

## Why It Failed
The total time for invariant checking is tiny. Looking at Tracy data:
- `processPostTxSetApply` total: 53ms/ledger (includes fee refunds + result/meta)
- The invariant checking portion is even smaller (a subset of the above)
- At ~1-5ms savings potential, this is lost in benchmark noise

Additionally, the original motivation to overlap fee refunds was proven impossible:
fee source accounts appear in Soroban footprints (e.g., SAC transfers modify
account balance via contract). `commitChangesToLedgerTxn` writes `mGlobalEntryMap`
entries back to LTX, overwriting any fee refund modifications made before it.
Only invariant checking (which doesn't modify LTX) could be moved, but its
time was negligible.

## Key Learning
Fee refunds CANNOT be moved before `commitChangesToLedgerTxn` because:
1. `preParallelApplyAndCollectModifiedClassicEntries` copies fee source accounts
   from LTX into `mGlobalEntryMap` (since they appear in Soroban footprints)
2. `commitChangesToLedgerTxn` writes `mGlobalEntryMap` back to LTX using
   `createWithoutLoading`/`updateWithoutLoading`
3. This overwrites any account balance changes from fee refunds

## Files Changed
- `src/ledger/LedgerManagerImpl.cpp` — inlined invariant checks in commit loop
- `src/ledger/LedgerManagerImpl.h` — updated function signatures
