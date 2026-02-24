# Experiment 059: Cache TTL key lookups in addReads

## Date
2026-02-24

## Hypothesis
`addReads` in `InvokeHostFunctionOpFrame.cpp` calls `getTTLKey(lk)` for every
soroban entry in every TX's footprint. `getTTLKey` does `xdr::xdr_to_opaque(e)`
(XDR serialization) + `sha256(...)` (SHA-256 hash) to compute the TTL key.
With ~256K soroban entries across 64K TXs, this costs ~300-600ns per entry.

The `ThreadParallelApplyLedgerState` already has a `mTTLKeyCache` that maps
soroban data/code keys to their pre-computed TTL keys (populated during
`collectClusterFootprintEntriesFromGlobal`). By making the parallel apply
helper use this cache instead of recomputing, we avoid SHA-256 + XDR per entry.

## Change Summary
1. **`ParallelApplyUtils.h`**: Added `lookupCachedTTLKey()` to
   `ThreadParallelApplyLedgerState` and `getCachedTTLKey()` to
   `TxParallelApplyLedgerState` for cache access.

2. **`ParallelApplyUtils.cpp`**: Implemented cache lookup methods.

3. **`InvokeHostFunctionOpFrame.cpp`**: Added virtual `computeTTLKey()` to
   `InvokeHostFunctionApplyHelper` (defaults to `getTTLKey`). Overridden in
   `InvokeHostFunctionParallelApplyHelper` to use the TTL key cache. Changed
   `addReads` to call `computeTTLKey` instead of `getTTLKey`.

## Results

### TPS
- Baseline: 18,944 TPS (experiment 058)
- Post-change run 1: 18,368 TPS (run-to-run variance)
- Post-change run 2: 18,944 TPS
- Delta: 0 TPS (flat — per-thread savings too small to move binary search)

### Tracy Analysis (averaged across two runs)
- `applyLedger` avg: ~992ms (baseline: 966ms) — within noise
- `addReads` self-time: ~220ms (baseline: 257ms) — **-37ms (-15%)**
- `addReads` mean per call: ~1,713ns (baseline: 2,007ns) — **-294ns (-15%)**
- `addReads` children (getLedgerEntryOpt, toCxxBuf): unchanged
- Other zones: within run-to-run noise

## Why TPS Didn't Change
The ~37ms savings across 4 threads is ~9ms per thread, representing only ~2%
of the ~462ms parallel wait time. This is below the binary search resolution
and within benchmark variance. The improvement compounds with other per-TX
optimizations on the parallel path.

## Files Changed
- `src/transactions/ParallelApplyUtils.h` — Added TTL key cache lookup methods
- `src/transactions/ParallelApplyUtils.cpp` — Implemented cache lookup
  delegation
- `src/transactions/InvokeHostFunctionOpFrame.cpp` — Added virtual
  computeTTLKey, parallel override using cache, changed addReads to use it

## Commit
08035dff3
