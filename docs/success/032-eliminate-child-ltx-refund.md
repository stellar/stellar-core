# Experiment 032: Eliminate Child LTX in refundSorobanFee

## Date
2026-02-21

## Hypothesis
`refundSorobanFee` creates a child `LedgerTxn` for every Soroban TX to provide
rollback semantics. However, the child LTX is unnecessary because `addBalance`
validates all constraints before modifying, and the subsequent operations
(`finalizeFeeRefund`, `feePool -= feeRefund`) cannot throw. Operating directly
on the parent LTX eliminates child LTX construction and commit overhead.

## Change Summary
Removed `LedgerTxn ltx(ltxOuter)` and `ltx.commit()` from `refundSorobanFee`.
All operations now use `ltxOuter` directly. Added a comment explaining why the
child LTX is unnecessary.

Safety analysis:
- `addBalance` checks overflow, min balance, and liabilities BEFORE modifying
  `acc.balance`. Returns false without modification on failure.
- `finalizeFeeRefund` sets a flag on txResult (cannot throw).
- `feePool -= feeRefund` is simple arithmetic (cannot throw).
- If `loadAccount` throws, no modifications have been made yet.

## Results

### TPS
- Baseline (exp-031): 14,976 TPS [14,976-15,104]
- Post-change: 15,168 TPS [15,168-15,232]
- Delta: +192 TPS (+1.3%)

### Tracy Analysis
| Zone | Exp-031 (ns/TX) | Exp-032 (ns/TX) | Delta |
|------|-----------------|-----------------|-------|
| refundSorobanFee | 1,497 | 1,275 | -222 (-14.8%) |

| Zone | Exp-031 (ms/ledger) | Exp-032 (ms/ledger) | Delta |
|------|---------------------|---------------------|-------|
| processPostTxSetApply | 35.2 | 31.0 | -4.2 (-11.9%) |
| applyLedger | 1222 | 1215 | -7 (-0.6%) |

## Files Changed
- `src/transactions/TransactionFrame.cpp` — Removed child LTX in
  `refundSorobanFee`, operate directly on `ltxOuter`

## Commit
