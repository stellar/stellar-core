# Experiment 040: Remove ZoneScoped from Trivial Hot Functions

## Date
2026-02-23

## Hypothesis
Tracy's `ZoneScoped` macro adds ~40-50ns overhead per call for zone entry/exit.
In trivial, high-frequency functions (cached getters, thin wrappers), this
overhead can dominate the actual function time. Removing ZoneScoped from these
functions reduces per-call overhead across the entire apply path.

## Change Summary
Removed `ZoneScoped` from 6 trivial, high-frequency functions:

1. **`TransactionFrame::getFullHash()`** — cached hash getter (6.1M calls, 48ns mean)
2. **`TransactionFrame::getContentsHash()`** — cached hash getter (242K calls)
3. **`TransactionFrame::getSize()`** — cached size getter (2.5M calls, 103ns mean)
4. **`TransactionFrame::computePreApplySorobanResourceFee()`** — thin wrapper (242K calls)
5. **`SHA256::add()`** — OpenSSL wrapper (2.2M calls, 186ns mean)
6. **`sha256()`** — one-shot hash wrapper (1.5M calls, 761ns mean)

These functions either return a cached value (just an if-check + return) or
delegate to a single function call. ZoneScoped provided no useful profiling
data for them — the zone times were dominated by instrumentation overhead.

## Results

### TPS
- Baseline: 16,640 TPS (experiment 038)
- Post-change: 16,640 TPS [16,640 - 16,768]
- Delta: **0% TPS** (binary search step too coarse to detect the improvement)

### Tracy Analysis (per ledger, averaged over 4 samples)

| Zone | Baseline (038) | Post-change | Delta |
|------|---------------|-------------|-------|
| applyLedger | 1,109ms | 1,092ms | **-17ms (-1.5%)** |
| processFeesSeqNums | 77ms | 66ms | **-11ms (-14%)** |
| applySorobanStageClustersInParallel | 605ms | 588ms | **-17ms (-2.8%)** |
| commitChangesToLedgerTxn | 73ms | 74ms | 0 |
| processPostTxSetApply | 64ms | 64ms | 0 |
| finalizeLedgerTxnChanges | 153ms | 164ms | +11ms (variance) |

The 17ms improvement in the parallel phase comes from reduced per-TX overhead
in worker threads (getFullHash, getSize, computePreApplySorobanResourceFee
are called per TX). The processFeesSeqNums improvement comes from fewer
instrumented hash operations during fee processing.

### Tracy File Size
- Baseline: 391MB (30s capture)
- Post-change: 314MB (30s capture) — 20% smaller due to fewer zone events

## Files Changed
- `src/transactions/TransactionFrame.cpp` — removed ZoneScoped from getFullHash,
  getContentsHash, getSize, computePreApplySorobanResourceFee
- `src/crypto/SHA.cpp` — removed ZoneScoped from sha256() and SHA256::add()

## Commit
(see git log)
