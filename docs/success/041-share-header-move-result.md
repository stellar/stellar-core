# Experiment 041: Share LedgerTxnHeader in processRefund + Move Result Pair

## Date
2026-02-23

## Hypothesis
In `processRefund`, the LedgerTxnHeader is loaded twice per TX: once inside
`refundSorobanFee` (for balance updates and feePool modification), and once
for the V23 event stage check. Sharing a single header load eliminates 16K
redundant header activate/deactivate cycles per ledger.

Additionally, in `processResultAndMeta`, the `TransactionResultPair` is copied
into the txResultSet vector. When meta is disabled (benchmark path), the local
copy is never used again, so we can move instead of copy.

## Change Summary
1. **Split `refundSorobanFee` into two functions**: The original public method
   loads the header and delegates to a new `refundSorobanFeeWithHeader` that
   accepts a pre-loaded `LedgerTxnHeader&`. This avoids re-loading the header.

2. **Shared header in processRefund**: `processRefund` now loads the header once
   and passes it to both `refundSorobanFeeWithHeader` and the V23 version check.
   This eliminates one `loadHeader()` call per TX (16K calls/ledger).

3. **Move semantics in processResultAndMeta**: When `ledgerCloseMeta` is null
   (benchmark path), use `std::move(resultPair)` for the emplace_back into
   txResultSet to avoid copying the TransactionResult.

## Results

### TPS
- Baseline: 16,640 TPS (experiment 040)
- Post-change: 16,640 TPS [16,640 - 16,768]
- Delta: **0% TPS** (binary search step too coarse)

### Tracy Analysis (per ledger)

| Zone | Baseline (040) | Post-change | Delta |
|------|---------------|-------------|-------|
| applyLedger | 1,092ms | 1,072ms | **-20ms (-1.8%)** |
| processPostTxSetApply | 64ms | 64ms | 0 |
| processRefund | 1,469ns/call | 1,494ns/call | ~0 |
| processResultAndMeta | 2,266ns/call | 2,295ns/call | ~0 |
| applySorobanStageClustersInParallel | 588ms | 588ms | 0 |
| commitChangesToLedgerTxn | 74ms | 72ms | -2ms |
| finalizeLedgerTxnChanges | 164ms | 157ms | -7ms |

The per-zone improvements in processPostTxSetApply are below the noise floor.
The applyLedger -20ms improvement may be from reduced cache pressure (fewer
header activations) or variance (only 3 samples vs 4 in baseline).

### Tracy File Size
- Baseline: 314MB (30s capture)
- Post-change: 297MB (30s capture) — 5% smaller (refundSorobanFee zone no
  longer emitted from the hot path since processRefund now calls the
  WithHeader variant directly)

## Files Changed
- `src/transactions/TransactionFrame.cpp` — split refundSorobanFee, shared
  header in processRefund
- `src/transactions/TransactionFrame.h` — added refundSorobanFeeWithHeader decl
- `src/ledger/LedgerManagerImpl.cpp` — move semantics in processResultAndMeta

## Commit
(see git log)
