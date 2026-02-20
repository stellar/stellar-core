# Experiment 011: Disable BUILD_TESTS Meta Tracking in Benchmark

## Date
2026-02-20

## Hypothesis
BUILD_TESTS forces meta tracking (`enableTxMeta=true`, `mLastLedgerTxMeta`
per-tx copies, `mLastLedgerCloseMeta` bulk copy) even when the benchmark
has no meta consumer. This overhead doesn't exist in production validators.
Disabling it should reduce apply time and make the benchmark more representative.

## Change Summary
Added `DISABLE_META_TRACKING_FOR_TESTING` config flag. When true (set
automatically for `max-sac-tps` mode):
1. Skips BUILD_TESTS `ledgerCloseMeta` creation when no meta stream is active
2. Does not force `enableTxMeta = true` (lets it follow production behavior)
3. Skips per-tx `mLastLedgerTxMeta.emplace_back()` deep copies (10.6K/ledger)
4. Skips bulk `mLastLedgerCloseMeta = *ledgerCloseMeta` deep copy

This makes the benchmark representative of validator nodes (which don't
stream meta in production).

## Results

### TPS
- Baseline: 10,688 TPS [10688, 10752]
- Post-change: 10,688 TPS [10688, 10752]
- Delta: 0% (binary search resolution of 64 TPS masks the improvement)

### Tracy Analysis (6-ledger totals, exp010.tracy vs exp011.tracy)

| Zone | Baseline | Post-change | Delta |
|------|----------|-------------|-------|
| applyLedger mean (ms/ledger) | 1,486.0 | 1,435.3 | **-50.7 (-3.41%)** |
| applyLedger stddev | 53.5 ms | 18.6 ms | Much tighter variance |
| processResultAndMeta (ms/call) | 2.134 us | 0.344 us | **-83.9%** |
| processPostTxSetApply total | 318.3 ms | 159.5 ms | **-49.9%** |
| processFeesSeqNums total | 438.5 ms | 341.0 ms | **-22.2%** |
| finalizeLedgerTxnChanges | 864.9 ms | 794.6 ms | **-8.1%** |

Savings breakdown per ledger:
- processResultAndMeta: ~19ms (meta finalize + copy eliminated)
- processFeesSeqNums: ~16ms (getChanges() + pushTxFeeProcessing skipped)
- finalizeLedgerTxnChanges: ~12ms (less meta data to finalize)
- Other: ~4ms (reduced allocator pressure, better cache behavior)

## Why TPS Didn't Change
The binary search uses 64-tx steps. A 50ms/ledger improvement at ~10.6K
txs/ledger equates to ~50 additional txs capacity, which is less than the
64-tx step. The improvement compounds with future optimizations.

## Key Learning
BUILD_TESTS meta tracking overhead was ~50ms/ledger (3.4% of apply time).
The overhead came from three sources:
1. Per-tx `mLastLedgerTxMeta` deep copies (XDR TransactionMeta for 10.6K txs)
2. Bulk `mLastLedgerCloseMeta` deep copy (entire close meta frame)
3. `getChanges()` in processFeesSeqNums (builds per-tx LedgerEntryChange diffs)

Additionally, `enableTxMeta=true` caused TransactionMetaBuilder operations
(setLedgerChanges, pushTxChanges, etc.) to do real work in parallel threads,
adding per-tx overhead that doesn't exist in production validators.

## Files Changed
- `src/main/Config.h` -- added DISABLE_META_TRACKING_FOR_TESTING flag
- `src/main/Config.cpp` -- default value and config parsing
- `src/main/CommandLine.cpp` -- set flag for max-sac-tps mode
- `src/ledger/LedgerManagerImpl.cpp` -- guarded 4 BUILD_TESTS meta blocks

## Commit
