# Experiment 070: upsertEntry KnownNew/KnownExisting fast paths — FAILED (0% change)

## Date
2026-02-24

## Hypothesis
By tracking which RW footprint keys have live entries in the pre-state during
`addReads`, we can skip the expensive `getLiveEntryOpt` lookup in `upsertEntry`
during `recordStorageChanges`. For new entries (the common case in this benchmark),
the full scope traversal always fails — using `upsertEntryKnownNew` skips it entirely.

## Changes
1. **`ParallelApplyUtils.h`**: Added `upsertEntryKnownNew` method to
   `TxParallelApplyLedgerState`, `LedgerAccessHelper`, and
   `ParallelLedgerAccessHelper` classes.
2. **`ParallelApplyUtils.cpp`**: Implemented `upsertEntryKnownNew` — same as
   `upsertEntryKnownExisting` but returns `true` (is a create).
3. **`InvokeHostFunctionOpFrame.cpp`**: Added `mRwKeyExistedBits` (uint64_t)
   and `mRwKeyExistedVec` (vector<bool>) to track which RW keys had live
   entries in the pre-state. Modified `addReads` to set these bits when
   entries are found. Modified `recordStorageChanges` to use three paths:
   - `foundInRwFootprint && existedInPreState` → `upsertLedgerEntryKnownExisting`
   - `foundInRwFootprint && !existedInPreState` → `upsertLedgerEntryKnownNew`
   - `!foundInRwFootprint` → fallback to `upsertLedgerEntry` (handles TTL entries)

## Benchmark Result
- Baseline: 19,520 TPS
- Experiment: 19,520 TPS [19,520, 19,648]
- Change: **0% (no measurable improvement)**

## Tracy Analysis
The per-call optimization was actually significant:

| Metric | Baseline | Exp 070 |
|--------|----------|---------|
| `upsertEntry` total self-time (30s) | 445ms (192K calls, 2318ns mean) | 211ms combined (3 variants) |
| `recordStorageChanges` per-call | 9717ns | 6069ns (-37.6%) |
| `applySorobanStageClustersInParallel` per-ledger | 550ms | 546ms (-4ms) |
| `applyLedger` per-ledger | 947ms | 947ms (0ms) |

The 234ms total self-time savings (per 30s trace) translates to ~58ms/ledger spread
across 4 threads = ~15ms/thread. This is too small relative to the ~550ms parallel
phase to produce a measurable TPS improvement.

## Key Learning
- `upsertEntry` self-time (445ms/30s = ~111ms/ledger across 4 threads = ~28ms/thread)
  is only ~5% of the parallel phase wall time per thread
- Even a 53% reduction in that self-time saves only ~15ms/thread
- TTL entries (64K/ledger) still use the generic `upsertEntry` path since they
  aren't in the RW footprint — only half the calls were optimized
- The parallel phase is dominated by Rust/Soroban host execution, not by
  `upsertEntry` lookups

## Conclusion
The optimization is technically correct and measurably reduces `recordStorageChanges`
time, but the absolute savings are too small to move the TPS needle. The critical
path is dominated by Soroban execution and post-parallel sequential work
(`finalizeLedgerTxnChanges` + `sealLedgerTxnAndStoreInBucketsAndDB`).
