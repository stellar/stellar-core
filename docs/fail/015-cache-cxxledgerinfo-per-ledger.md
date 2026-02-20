# Experiment 015: Cache CxxLedgerInfo Per Ledger Close

## Date
2026-02-20

## Hypothesis
`getLedgerInfo()` in `InvokeHostFunctionParallelApplyHelper` rebuilds a
`CxxLedgerInfo` struct per transaction by re-serializing `cpuCostParams` and
`memCostParams` via `xdr::xdr_to_opaque` and copying `network_id` byte-by-byte.
All these values are constant for the entire ledger close. At ~14K TXs/ledger,
this means ~28K unnecessary XDR serializations + ~28K heap allocations (via
`toCxxBuf`). Pre-serializing cost params once in `ParallelLedgerInfo` and
copying pre-built byte vectors in `getLedgerInfo()` should eliminate the per-TX
XDR serialization overhead.

## Change Summary
- Extended `ParallelLedgerInfo` to cache pre-serialized CPU/mem cost param
  bytes (as `std::vector<uint8_t>`) and soroban config scalars (memory_limit,
  TTL settings) at construction time.
- Added `ParallelLedgerInfo::buildCxxLedgerInfo()` method that constructs
  `CxxLedgerInfo` from cached bytes (vector copy, not XDR serialization).
- Updated `InvokeHostFunctionParallelApplyHelper::getLedgerInfo()` to call
  `mLedgerInfo.buildCxxLedgerInfo()` instead of the old
  `stellar::getLedgerInfo(sorobanConfig, ...)`.
- Threaded `SorobanNetworkConfig const&` through `applySorobanStage` →
  `getParallelLedgerInfo`.

## Results

### TPS
- Baseline: 14,144 TPS (experiment 016)
- Post-change: 13,888 TPS [13,888 - 13,952]
- Delta: -256 TPS (-1.8%, within noise)

### Tracy Analysis (exp012-confirm baseline vs exp013)

| Zone | Baseline (per-call) | Exp013 (per-call) | Delta |
|------|---------------------|-------------------|-------|
| `invoke_host_function` self | 23,672 ns | 23,188 ns | -2.0% |
| `applySorobanStageClustersInParallel` self/call | 854ms | 840ms | -1.6% |

## Why It Failed
The `ContractCostParams` XDR structure is small (~30 entries × 3 int32 fields
= ~360 bytes). Serializing 360 bytes via `xdr_to_opaque` takes only ~500ns-1µs.
With 2 calls per TX, the total overhead is ~1-2µs per TX.

However, the change still requires constructing a new `CxxBuf` per TX (heap
allocation via `make_unique<vector<uint8_t>>` + vector copy), costing ~200ns
each. So the net savings is only ~0.6-1.4µs per TX — about 0.5% of the
~293µs total per-TX cost.

On 4 threads processing ~14K TXs, this translates to ~4ms wall-clock savings
per ledger — too small to cross the 64-TX binary search step.

### Key Lesson
CxxBuf's ownership model (UniquePtr<CxxVector<u8>>) requires a heap allocation
per use regardless. Caching serialized bytes avoids only the XDR walk, not the
allocation. For small XDR types like `ContractCostParams`, the walk is cheap
and the optimization is near-zero impact.

To meaningfully reduce per-TX overhead in the C++ → Rust bridge, the bridge
interface would need to accept shared/borrowed cost params rather than
per-invocation owned copies — a larger architectural change.

## Files Changed
- `src/transactions/ParallelApplyUtils.h` — extended ParallelLedgerInfo
- `src/transactions/ParallelApplyUtils.cpp` — constructor + buildCxxLedgerInfo
- `src/transactions/InvokeHostFunctionOpFrame.cpp` — use buildCxxLedgerInfo
- `src/ledger/LedgerManagerImpl.h` — updated applySorobanStage signature
- `src/ledger/LedgerManagerImpl.cpp` — pass sorobanConfig through call chain
