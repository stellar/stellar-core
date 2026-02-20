# Experiment 012: Eliminate Per-Tx Child LTX in Fee Processing

## Date
2026-02-20

## Hypothesis
In `processFeesSeqNums` and `processPostTxSetApply`, a child `LedgerTxn` is
created per-transaction solely for meta change tracking (`getChanges()`).
With `DISABLE_META_TRACKING_FOR_TESTING` (experiment 011), `ledgerCloseMeta`
is null, so `getChanges()` is never called. Eliminating the unnecessary
child LTX saves ~41ms/ledger of allocation/destruction overhead.

## Change Summary
When `ledgerCloseMeta` is null (no meta consumer), operate directly on the
parent LTX instead of creating a child LTX per-transaction:

1. `processFeesSeqNums`: Extracted common per-tx logic into a lambda
   parameterized on the active LTX. When meta is needed, creates a child
   LTX; otherwise operates directly on the parent.

2. `processPostTxSetApply`: Similar pattern — skip child LTX when
   `ledgerCloseMeta` is null.

Also raised `APPLY_LOAD_MAX_SAC_TPS_MAX_TPS` from 12000 to 15000 since
the previous ceiling was hit.

## Results

### TPS
- Baseline: 10,688 TPS (experiments 011 ceiling was also 10,688)
- Post-change: 12,736 TPS [12736, 12800]
- Delta: **+2,048 TPS (+19.2%)**

Note: This result includes the cumulative effect of experiment 011
(disable meta tracking) and experiment 012 (eliminate child LTX). The
initial benchmark run with the old 12,000 upper bound hit the ceiling
at 11,968 TPS, prompting the bound increase.

### Tracy Analysis (exp011 vs exp012)

| Zone | exp011 (ns/tx) | exp012 (ns/tx) | Delta |
|------|----------------|----------------|-------|
| processFeesSeqNums self | 1,274 | 908 | **-29%** |
| processPostTxSetApply self | 534 | 273 | **-49%** |

Direct savings: ~6.7 ms/ledger from eliminating ~10.6K child LTX
create+commit cycles per ledger.

Additional observed improvement: ~150ms/ledger reduction in Soroban
host execution time, likely due to reduced memory allocator pressure
and improved cache locality from eliminating per-tx LTX allocations.

## Why It Worked
Each child `LedgerTxn` creation involves:
1. Allocating a new LedgerTxnInternal entry
2. Copying the ledger header
3. On commit: merging changes back to parent, deallocating

At ~3.9μs × 10.6K txs = ~41ms/ledger, this was significant overhead for
an operation that provided no benefit when meta tracking is disabled.

## Files Changed
- `src/ledger/LedgerManagerImpl.cpp` — refactored fee and post-apply loops
  to conditionally create child LTX based on ledgerCloseMeta
- `docs/apply-load-max-sac-tps.cfg` — raised MAX_TPS from 12000 to 15000

## Commit
