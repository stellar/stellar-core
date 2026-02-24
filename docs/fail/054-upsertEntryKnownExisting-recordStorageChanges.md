# Experiment 054: Use upsertEntryKnownExisting in recordStorageChanges

## Date
2026-02-24

## Hypothesis
`upsertEntry` in `TxParallelApplyLedgerState` calls `getLiveEntryOpt` (traversing
TX->thread->global scope chain, ~700-800ns per hash lookup) to detect creates vs
updates. For entries known to be live (pre-existing), this check is redundant. By
tracking whether all Soroban footprint entries were live during `addReads` and
routing to `upsertEntryKnownExisting` (which skips `getLiveEntryOpt`), we could
save ~700ns * 3 entries * 16K TXs = ~34ms per stage.

## Change Summary
- Added `mAllFootprintEntriesLive` flag to `InvokeHostFunctionApplyHelper`
- Set flag to false in `addReads` when any Soroban entry lacks a live TTL
- Modified `recordStorageChanges` to call `upsertLedgerEntryKnownExisting` when
  flag is true, bypassing `getLiveEntryOpt` existence check

## Results

### TPS
- Baseline: 17,984 TPS
- Post-change: 18,368 TPS
- Delta: +384 TPS (+2.1%) — within variance

### Tracy Analysis
- `upsertEntry` self-time: 442ms (baseline 445ms) — unchanged
- `upsertEntryKnownExisting`: 0 calls — fast path never taken
- The `mAllFootprintEntriesLive` flag was false for 100% of TXs

## Why It Failed

The benchmark creates **new destination contract addresses every ledger**:
```cpp
SCAddress toAddress(SC_ADDRESS_TYPE_CONTRACT);
toAddress.contractId() = sha256(
    fmt::format("dest_{}_{}", i, lm.getLastClosedLedgerNum()));
```

This means the receiver's CONTRACT_DATA balance entry doesn't exist yet, so:
1. TTL lookup for receiver's balance returns nullopt
2. `sorobanEntryLive` stays false
3. `mAllFootprintEntriesLive` set to false for every TX

The slow `upsertLedgerEntry` path is **correct** for this workload — the host
genuinely creates new balance entries and corresponding TTL entries for each TX.
The create-detection logic (counting created Soroban vs TTL entries for pairing
validation) cannot be bypassed.

A per-key approach (tracking which specific keys are live) would add overhead
comparable to the `getLiveEntryOpt` check being avoided.

## Files Changed
- `src/transactions/InvokeHostFunctionOpFrame.cpp` — Added flag, conditional
  upsert path (reverted)
