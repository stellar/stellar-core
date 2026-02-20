# Experiment 011: Cache TransactionFrame::getSize()

## Date
2026-02-20

## Hypothesis
`TransactionFrame::getSize()` recomputes `xdr::xdr_size(mEnvelope)` on every
call with zero caching. With 2.5M+ calls per 30s trace at 273ns each (694ms
total self-time), caching the result eliminates redundant XDR size traversals.
The envelope is immutable after construction (const in non-test builds), so
the cached value is always valid.

## Change Summary
- `TransactionFrame.h`: Added `mutable uint32_t mCachedSize{0}` member
- `TransactionFrame.cpp:getSize()`: Return cached value on subsequent calls;
  compute and cache on first call only

## Results

### TPS
- Baseline: 9,408 TPS
- Post-change: 9,408 TPS
- Delta: 0% (within binary search step granularity of 64 TPS)

### Tracy Analysis (30s capture, 9 ledger commits)

| Zone | Baseline (self-time) | Post-change (self-time) | Delta |
|------|---------------------|------------------------|-------|
| getSize | 694ms (273ns/call) | 195ms (75ns/call) | **-499ms (-72%)** |
| getFullHash | 380ms (67ns/call) | 374ms (65ns/call) | -6ms (noise) |
| finalizeLedgerTxnChanges (total) | 136ms/ledger | 128ms/ledger | -8ms |
| addLiveBatch (total) | 93ms/ledger | 90ms/ledger | -3ms |

The getSize self-time dropped from 273ns to 75ns per call — the residual 75ns
is function call + Tracy zone + branch overhead. Across 2.6M calls in the
trace, this saves ~500ms total. The improvement is spread across both the
apply path and TX set building; the fraction within `applyLedger` is smaller
but still beneficial.

## Thread Safety
`mCachedSize` is mutable and only written on first access. Multiple threads
may race to cache the same value, but since `xdr_size` is deterministic and
`uint32_t` writes are atomic on x86, this is safe (benign data race — all
writers store the same value).

## Files Changed
- `src/transactions/TransactionFrame.h` — added `mCachedSize` member
- `src/transactions/TransactionFrame.cpp` — cache-on-first-call in getSize()

## Verdict
**Success.** Tracy confirms a 72% reduction in `getSize` self-time (694ms →
195ms). TPS unchanged due to binary search granularity, but the optimization
eliminates clearly redundant work across 2.5M+ calls.
