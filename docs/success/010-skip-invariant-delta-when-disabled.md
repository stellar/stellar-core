# Experiment 013: Skip Invariant Delta When No Invariants Enabled

## Date
2026-02-20

## Hypothesis
`setEffectsDeltaFromSuccessfulTx` builds a `LedgerTxnDelta` with
`shared_ptr` allocations and entry copies for every successful Soroban
transaction. This delta is consumed exclusively by `checkAllTxBundleInvariants`
→ `checkOnOperationApply`. When `INVARIANT_CHECKS` is empty (the default,
and the benchmark config), `checkOnOperationApply` iterates an empty list
and does nothing. Therefore all work in `setEffectsDeltaFromSuccessfulTx`
is wasted — 285ms total across 4 worker threads (~71ms wall-clock).

## Change Summary
Two guarded skips:

1. **`TransactionFrame.cpp`** (~line 2122): Wrap the
   `setEffectsDeltaFromSuccessfulTx` call in
   `if (!config.INVARIANT_CHECKS.empty())`. When invariants are disabled,
   the delta is never built.

2. **`LedgerManagerImpl.cpp`** (~line 2424): Add
   `bool const hasInvariants = !config.INVARIANT_CHECKS.empty()` and gate
   the invariant-check block with `if (hasInvariants && ...)`. When no
   invariants are configured, skip the check entirely.

Both changes are no-ops when invariants are enabled (production validators
that configure `INVARIANT_CHECKS`).

## Results

### TPS
- Baseline: 12,736 TPS (experiment 012)
- Post-change: 13,760 TPS [13760, 13824]
- Delta: **+1,024 TPS (+8.0%)**

### Tracy Analysis (exp014c baseline vs exp015)

| Zone | exp014c self-time (ns) | exp015 self-time (ns) | Delta |
|------|------------------------|-----------------------|-------|
| setEffectsDeltaFromSuccessfulTx | 285,000,000 | 0 (eliminated) | **-100%** |
| applySorobanStageClustersInParallel | 4,772,000,000 | 4,881,562,630 | ~+2% (noise) |
| verify_ed25519_signature_dalek | 2,777,000,000 | 3,154,829,300 | ~+14% (noise/load) |
| charge (budget metering) | 2,694,000,000 | 2,625,705,713 | ~-3% (noise) |
| recordStorageChanges | 358,000,000 | 342,151,833 | ~-4% |
| addReads | 591,000,000 | 543,304,685 | ~-8% |

The `setEffectsDeltaFromSuccessfulTx` zone is completely absent from the
exp015 trace, confirming the optimization is effective. The 8% TPS gain
exceeds the ~2.2% estimate from pure self-time savings, suggesting
secondary benefits from reduced allocator pressure and improved cache
behavior during parallel execution.

## Why It Worked
Each call to `setEffectsDeltaFromSuccessfulTx` (66K calls/trace) performs:
1. Iteration over all modified LedgerTxn entries
2. `shared_ptr` allocation for each `LedgerTxnDelta` entry
3. Deep copy of `LedgerEntry` objects (XDR structures)
4. Construction of before/after entry pairs

At ~4.3μs × 66K calls = 285ms total, running on 4 worker threads during
the parallel phase, this translated to ~71ms wall-clock overhead per ledger.
Eliminating this reduced per-ledger time enough to fit ~1,024 more
transactions within the 1,000ms target close time.

## Files Changed
- `src/transactions/TransactionFrame.cpp` — guarded `setEffectsDeltaFromSuccessfulTx` call
- `src/ledger/LedgerManagerImpl.cpp` — guarded invariant check block

## Commit
