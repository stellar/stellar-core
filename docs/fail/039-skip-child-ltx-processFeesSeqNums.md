# Experiment 039: Skip Child LTX in processFeesSeqNums

## Date
2026-02-23

## Hypothesis
The child LTX in processFeesSeqNums (~4.4ms commit cost) could be eliminated
when meta tracking is disabled, similar to experiment 038's preParallelApply
optimization. Additionally, skipping merge-op tracking for Soroban TXs and
caching the ledger version check could save per-TX overhead.

## Change Summary
1. **Attempted: Eliminate outer child LTX** when `ledgerCloseMeta == nullptr`
   - FAILED: `applyLedger` holds an active `LedgerTxnHeader` on `ltx` (line 1486),
     preventing any `loadHeader()` call on the same LTX. The child LTX is required
     to provide a fresh header context for `processFeeSeqNum`.
   - Reverted this part.

2. **Implemented: Skip merge-op tracking for Soroban TXs** (`!tx->isSoroban()`)
   - Soroban TXs only have InvokeHostFunction ops, never ACCOUNT_MERGE
   - Saves `accToMaxSeq.emplace()` + `mergeOpInTx()` per TX (~0.25us/TX)

3. **Implemented: Cache ledger version outside loop**
   - Avoids per-TX `activeLtx.loadHeader()` for the V19 check
   - Saves ~0.1us/TX

4. **Implemented: Diagnostic Tracy zones** for commitChangesToLedgerTxn and
   processPostTxSetApply

## Results

### TPS
- Baseline: 16,640 TPS (experiment 038)
- Post-change: 16,640 TPS [16,640 - 16,768]
- Delta: **0% / 0 TPS**

### Tracy Analysis (per ledger, averaged over 4 samples)

| Zone | Baseline | Post-change | Delta |
|------|----------|-------------|-------|
| applyLedger | 1,109ms | 1,113ms | +4ms (noise) |
| processFeesSeqNums | 77ms | 73ms | -4ms |
| processFeesSeqNums: commit | N/A | 4.4ms | (new zone) |
| commitChangesToLedgerTxn | 73ms | 74ms | +1ms (noise) |
| processPostTxSetApply | 64ms | 63ms | -1ms (noise) |

### Diagnostic Zone Data (new)

| Zone | Total Time | Self Time | Notes |
|------|-----------|-----------|-------|
| commitChangesToLedgerTxn: upsert loop | 74ms | ~74ms | Almost all time in upsert |
| processPostTxSetApply: refund loop | 63ms | ~63ms | Almost all time in refund |

## Why It Failed

1. **Child LTX elimination blocked**: The parent `applyLedger` holds an active
   `LedgerTxnHeader` reference (`auto header = ltx.loadHeader()` at line 1486).
   This prevents any code operating on the same LTX from calling `loadHeader()`.
   The child LTX provides isolation from this constraint.

2. **Micro-optimizations too small**: The merge-op skip saves ~4ms total out of
   1,113ms applyLedger (~0.4%). Not enough to impact the binary search TPS result.

3. **Merge-op tracking was already cheap**: With 16K Soroban TXs,
   `accToMaxSeq.emplace()` and `mergeOpInTx()` iterate small xdr::xvector<Operation>
   (1 op per Soroban TX). The map operations are the main cost but still tiny at
   ~0.25us/TX.

## Key Learnings

- The `LedgerTxnHeader` active-reference mechanism prevents eliminating child
  LTXs when any ancestor holds a header reference. This is a fundamental
  constraint of the LedgerTxn design.
- `processFeesSeqNums: commit` is 4.4ms/ledger — not a significant target.
- `commitChangesToLedgerTxn` spends essentially all 74ms in the upsert loop
  (copying entries into the parent LTX via `updateWithoutLoading`).
- `processPostTxSetApply` spends essentially all 63ms in the refund loop.

## Files Changed (reverted)
- `src/ledger/LedgerManagerImpl.cpp` -- processFeesSeqNums optimization + Tracy zones
- `src/transactions/ParallelApplyUtils.cpp` -- diagnostic Tracy zone
