# Experiment 049: Skip Child LTX in processFeesSeqNums

## Date
2026-02-23

## Hypothesis
`processFeesSeqNums` (66.8ms/ledger) unconditionally creates a child `LedgerTxn`
wrapping all ~17K account fee modifications. When meta tracking is disabled
(benchmark path), this child LTX is only needed to provide isolation from the
parent's active `LedgerTxnHeader` — but that header can be deactivated
explicitly. Eliminating the child LTX avoids: child creation (~1ms), commit
overhead copying 17K entries from child to parent map (4.5ms), and the cost of
each account load traversing child-to-parent chain (~1-2ms).

Previous Experiment 039 attempted this but failed because the parent
`applyLedger` holds an active `LedgerTxnHeader`, and `loadHeader()` inside
processFeesSeqNums throws on the same LTX. This experiment solves it by
explicitly deactivating the header in the caller before the call.

## Change Summary
1. In `applyLedger`, added `header.deactivate()` before calling
   `processFeesSeqNums`. The header isn't needed after line ~1604 anyway.
   When meta is enabled, `processFeesSeqNums` creates a child LTX which
   would have deactivated it via `addChild()` anyway.
2. In `processFeesSeqNums`, made the child LTX conditional on
   `ledgerCloseMeta != nullptr`. When meta is disabled (benchmark path),
   operates directly on `ltxOuter`, avoiding child creation and commit.

## Results

### TPS
- Baseline: 17,216 TPS
- Post-change: 17,216 TPS
- Delta: 0% / 0 TPS (within noise — improvement too small for binary search)

### Tracy Analysis
- `processFeesSeqNums`: 66.8ms → 60.4ms per ledger (-9.6%)
- `processFeesSeqNums: commit`: 4.5ms → eliminated
- `applyLedger`: 1050.9ms → 1046.8ms per ledger (-0.4%)

## Files Changed
- `src/ledger/LedgerManagerImpl.cpp` — deactivate header before processFeesSeqNums; conditional child LTX creation

## Commit
<pending>
