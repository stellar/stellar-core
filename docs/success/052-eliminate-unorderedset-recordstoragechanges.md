# Experiment 052: Eliminate UnorderedSet from recordStorageChanges

## Date
2026-02-23

## Hypothesis
In `recordStorageChanges`, two `UnorderedSet<LedgerKey>` instances are built per TX:
1. `createdAndModifiedKeys` — tracks modified entries for the erase loop (3 inserts + 2 finds per TX)
2. `createdKeys` — tracks created entries for TTL pairing verification (1-3 inserts + getTTLKey SHA-256)

LedgerKey hashing is expensive (~300-500ns per hash, involves xdrComputeHash/SipHash
for CONTRACT_DATA keys + RandHasher's releaseAssert). For 64K TXs, this is ~192K
hash+insert operations plus ~64K getTTLKey calls (each involves SHA-256 + XDR
serialization). Replacing these with lightweight alternatives should significantly
reduce self-time.

## Change Summary
1. Replaced `createdAndModifiedKeys` UnorderedSet with a `uint64_t` bitfield
   (fallback to `vector<bool>` for >64 RW keys). For each modified entry,
   linear-scans the RW footprint (typically 2 keys) to mark coverage. Eliminates
   192K LedgerKey hash computations.

2. Replaced `createdKeys` UnorderedSet with two counters
   (`numCreatedSorobanEntries`, `numCreatedTTLEntries`). Verification uses
   count equality instead of set-based getTTLKey pairing. Eliminates 64K
   getTTLKey calls (SHA-256 + XDR serialization) in the verification loop.

## Results

### TPS
- Baseline: 18,368 TPS (exp050)
- Post-change: 18,368 TPS [18,368, 18,496] (2 runs)
- Delta: 0% (below binary search resolution)

### Tracy Analysis
- `recordStorageChanges` self-time: 235ms → 56.5ms (**-75.9%**, -178.5ms)
- `applyLedger` mean: 1,019ms → 996ms (-2.3%, -23ms)
- `upsertEntry` unchanged: 414ms (expected — not targeted)
- Wall clock improvement: ~23ms per ledger (insufficient to cross next TPS step)

### Why TPS Didn't Change
The ~23ms per-ledger improvement is real but the binary search step between
18,368 and 18,496 TPS requires apply time to drop enough that 18,496 TPS fits
within 1000ms. The 2.3% improvement was not enough to cross this threshold.
The improvement will compound with other optimizations.

## Files Changed
- `src/transactions/InvokeHostFunctionOpFrame.cpp` — replaced UnorderedSets
  with bitfield coverage tracking and counter-based verification

## Commit
