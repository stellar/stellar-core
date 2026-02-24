# Experiment 033: Optimize processFeesSeqNums Hot Loop

## Date
2026-02-21

## Hypothesis
The per-TX loop in processFeesSeqNums has overhead from:
1. Redundant `protocolVersionStartsFrom(activeLtx.loadHeader()...)` per TX
2. `mergeOpInTx` call for Soroban TXs that can never have merge ops
3. `std::map<AccountID, SequenceNumber>` with O(log n) insertion

Caching the V19 check, skipping mergeOpInTx for Soroban, and using
`std::unordered_map` should save ~6-10ms per ledger.

## Change Summary
1. Cached `isV19` before the loop to avoid per-TX loadHeader + comparison
2. Added `!tx->isSoroban()` guard before `mergeOpInTx` call
3. Changed `std::map<AccountID, SequenceNumber>` to `std::unordered_map`

## Results

### TPS
- Baseline (exp-032): 15,168 TPS [15,168-15,232]
- Post-change: 14,976 TPS [14,976-15,104]
- Delta: Within variance

### Tracy Analysis
| Zone | Exp-032 (ms/ledger) | Exp-033 (ms/ledger) | Delta |
|------|---------------------|---------------------|-------|
| processFeesSeqNums | 75.1 | 75.8 | +0.7 (noise) |
| applyLedger | 1215 | 1217 | +2 (noise) |

## Why It Failed
The optimized operations were already fast:
- `loadHeader()` in the LTX is a cached lookup (~50-100ns)
- `mergeOpInTx` for SAC TXs only checks 1 operation (~30ns)
- The `std::map` savings (~2.6ms) were lost in benchmark noise

The 75ms in processFeesSeqNums is dominated by `processFeeSeqNum` itself
(2.9µs/TX × 16K = 46ms) and the commit (4.6ms), not loop overhead.

## Files Changed (REVERTED)
- `src/ledger/LedgerManagerImpl.cpp`
