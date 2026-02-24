# Experiment 058: Cache CxxLedgerInfo cost params per ledger

## Date
2026-02-24

## Hypothesis
`getLedgerInfo()` in `InvokeHostFunctionOpFrame.cpp` creates a `CxxLedgerInfo`
struct per TX, including two `toCxxBuf()` calls that XDR-serialize the CPU and
memory cost parameters. These cost params are identical for all TXs in a ledger
but are re-serialized ~64K times per ledger (once per TX across 4 threads).

Each `toCxxBuf` call does `xdr::xdr_to_opaque()` which allocates a vector and
serializes ~20+ cost param entries. By pre-serializing the cost params once per
ledger in `ParallelLedgerInfo` and copying the pre-serialized bytes per TX
(a cheap memcpy vs full XDR serialization), we eliminate redundant work.

## Change Summary
1. **`ParallelApplyUtils.h`**: Added `cacheSorobanConfig()` method and cached
   fields to `ParallelLedgerInfo` (pre-serialized cost param vectors + scalar
   config values).

2. **`ParallelApplyUtils.cpp`**: Implemented `cacheSorobanConfig()` which
   pre-serializes CPU and memory cost params via `xdr::xdr_to_opaque()`.

3. **`LedgerManagerImpl.cpp`**: Modified `getParallelLedgerInfo()` to accept
   `SorobanNetworkConfig` and call `cacheSorobanConfig()`. Threaded the config
   through `applySorobanStage()`.

4. **`LedgerManagerImpl.h`**: Updated `applySorobanStage()` declaration.

5. **`InvokeHostFunctionOpFrame.cpp`**: Added `getLedgerInfoFromCache()` that
   constructs `CxxLedgerInfo` from pre-serialized bytes (vector copy instead
   of XDR serialization). Modified the parallel apply helper's `getLedgerInfo()`
   to use the cached version.

## Results

### TPS
- Baseline: 18,944 TPS (experiment 057)
- Post-change: 18,944 TPS
- Delta: 0 TPS (flat — parallel execution dilutes per-TX savings)

### Tracy Analysis
- `applyLedger` avg: 966ms (baseline: 987ms) — **-21ms (-2.1%)**
- `serialize inputs` total: 117ms (baseline: 217ms) — **-100ms (-46%)**
- `applySorobanStageClustersInParallel` self-time: 1,851ms (baseline: 1,986ms) — **-135ms (-6.8%)**
- `commitChangesFromThread` self-time: 130ms (baseline: 128ms) — flat
- `commitChangesToLedgerTxn` self-time: 123ms (baseline: 120ms) — flat
- `upsertEntry` cumulative self-time: 422ms (baseline: 425ms) — flat
- `updateState` self-time: 298ms (baseline: 299ms) — flat
- `addLiveBatch` avg: ~114ms (baseline: ~112ms) — flat

## Why TPS Didn't Change Despite 46% Serialize Improvement
The serialization happens on worker threads during parallel execution. The 100ms
total savings is spread across 4 threads (~25ms per thread). Since TPS is gated
by the slowest ledger close time (wall-clock), and the parallel section is not
the sole bottleneck, the per-thread savings of ~25ms wasn't enough to move the
needle on the binary search. The improvement compounds with other thread-side
optimizations.

## Files Changed
- `src/transactions/ParallelApplyUtils.h` — Added cached soroban config fields
  and methods to ParallelLedgerInfo
- `src/transactions/ParallelApplyUtils.cpp` — Added cacheSorobanConfig
  implementation, xdrpp/marshal.h include
- `src/ledger/LedgerManagerImpl.h` — Updated applySorobanStage declaration
- `src/ledger/LedgerManagerImpl.cpp` — Thread SorobanNetworkConfig to
  getParallelLedgerInfo, call cacheSorobanConfig
- `src/transactions/InvokeHostFunctionOpFrame.cpp` — Added
  getLedgerInfoFromCache, modified parallel helper to use cached data

## Commit
TBD
