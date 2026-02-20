# Experiment 013: Eliminate Child LTX in commonPreApply

## Date
2026-02-20

## Hypothesis
In `commonPreApply` (called via `preParallelApply` for each Soroban tx before
parallel execution), a child `LedgerTxn` is created per-transaction for meta
change tracking via `pushTxChangesBefore()`. With meta disabled, this child
LTX is unnecessary. Eliminating ~12.7K child LTX create+commit cycles should
save ~50ms/ledger of serial pre-apply overhead.

## Change Summary
- Added `isEnabled()` method to `TransactionMetaBuilder`
- In `commonPreApply`, conditionally skip child LTX creation when
  `meta.isEnabled()` returns false: operate directly on parent LTX for
  `commonValid`, `processSeqNum`, and `processSignatures`

## Results

### TPS
- Baseline: 12,736 TPS [12,736, 12,800]
- Post-change: 10,944 TPS [10,944, 11,008]
- Delta: **-1,792 TPS (-14.1%)** REGRESSION

### Tracy Analysis
Per-tx `preParallelApply` cost decreased slightly (11.3μs → 10.5μs), but
the parallel execution phase (`applySorobanStageClustersInParallel`) increased
by ~50ms/ledger (890ms → 940ms), causing an overall regression.

## Why It Failed
The child LTX elimination improved the serial pre-apply per-tx cost as
expected, but caused a significant regression in the parallel execution phase
that follows. The mechanism is unclear — possibly related to:

1. **LTX internal map structure**: Operating directly on the parent LTX
   (instead of via child LTX commit) may change the internal map structure
   in a way that affects `GlobalParallelApplyLedgerState` construction or
   subsequent parallel thread access patterns.
2. **Cache/memory effects**: Different allocation patterns from skipping
   child LTX may worsen cache locality for the subsequent parallel phase.
3. **Compiler optimization**: The restructured code (ternary with unique_ptr)
   may have affected inlining or branch prediction in hot paths.

Key insight: child LTX create+commit is not purely overhead — the child LTX
may provide beneficial isolation that improves cache locality or memory access
patterns for subsequent operations.

## Files Changed (reverted)
- `src/transactions/TransactionFrame.cpp` — conditional child LTX in commonPreApply
- `src/transactions/TransactionMeta.h` — added isEnabled() accessor
- `src/transactions/TransactionMeta.cpp` — isEnabled() implementation
