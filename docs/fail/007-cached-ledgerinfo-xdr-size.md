# Experiment 007: Cache CxxLedgerInfo + Remove Redundant xdr_size

## Status: MARGINAL / NOT WORTH PURSUING

## Hypothesis
`getLedgerInfo()` is called once per-tx in `InvokeHostFunctionParallelApplyHelper`
and re-serializes CPU/memory cost params (~3.2KB total) from XDR for every
transaction, even though these are identical for all transactions in a ledger.
Additionally, `xdr::xdr_size(lk)` is called in `addReads` and
`recordStorageChanges` for purely diagnostic metrics that are disabled in the
benchmark.

Expected savings: ~3-5µs/tx from CxxLedgerInfo caching + ~1-2µs/tx from
xdr_size removal = ~4-7µs/tx → ~2-3% TPS improvement.

## Implementation
1. **CxxLedgerInfo caching**: Added cached serialized cost param byte vectors
   (`mCachedCpuCostParamsBytes`, `mCachedMemCostParamsBytes`) to
   `ThreadParallelApplyLedgerState`, serialized once at thread construction.
   Modified `InvokeHostFunctionParallelApplyHelper::getLedgerInfo()` to clone
   cached bytes instead of calling `toCxxBuf(cpu)`/`toCxxBuf(mem)`.

2. **addReads xdr_size deferral**: Deferred `xdr::xdr_size(lk)` computation to
   only when `meterDiskReadResource` is actually called (not called for soroban
   entries on protocol 25+).

3. **recordStorageChanges xdr_size skip**: Skip `xdr::xdr_size(lk)` when
   `mDisableMetrics` is true (benchmark path), since keySize only feeds
   diagnostic metric `mMaxReadWriteKeyByte`.

### Files Modified
- `src/transactions/ParallelApplyUtils.h` — Added cached byte vector members
  and getter methods to `ThreadParallelApplyLedgerState`
- `src/transactions/ParallelApplyUtils.cpp` — Initialize cached bytes in
  constructor, implement getters, added `<xdrpp/marshal.h>` include
- `src/transactions/InvokeHostFunctionOpFrame.cpp` — Modified
  `InvokeHostFunctionParallelApplyHelper` to store thread state reference,
  override `getLedgerInfo()` with cached-byte version, defer xdr_size in
  `addReads`, skip xdr_size in `recordStorageChanges`

## Results
- **Baseline**: 9,408 TPS (exp002)
- **Experiment**: 9,536 TPS [9,536 — 9,664]
- **Change**: +1.4% (+128 TPS)
- **Tracy trace**: `/mnt/xvdf/tracy/exp007-cached-ledgerinfo.tracy`
- **Tests**: All 67 `[soroban][tx]` tests pass (49,254 assertions)

## Why the Impact Was Small
1. **CxxLedgerInfo serialization is not as expensive as estimated**: The actual
   XDR serialization of `ContractCostParams` (79 entries × 3 fields each) takes
   ~2-3µs. Cloning the cached bytes (memcpy of ~1.6KB) takes ~0.5µs. Net
   savings per-tx is only ~1.5-2.5µs.

2. **Heap allocation remains**: Each `CxxLedgerInfo` still requires 2 heap
   allocations for the `CxxBuf` `unique_ptr<vector<uint8_t>>` wrappers, even
   when cloning from cached bytes. The allocation overhead is a significant
   fraction of the total cost.

3. **xdr_size calls are cheap**: `xdr_size` for a LedgerKey is a simple
   traversal of a small XDR structure (~50-100ns). Even removing it from all
   ~16K calls per ledger saves only ~1ms.

4. **Savings are dwarfed by Rust-side overhead**: The ~226µs Rust time per-tx
   dominates. Saving 3-5µs on the C++ side is <2% of total per-tx time.

## Key Learning
- Individual C++ micro-optimizations on the per-tx path yield diminishing
  returns when the Rust side accounts for 86% of per-tx time (~226µs of ~262µs).
- To achieve significant TPS gains (>10%), optimizations must either:
  (a) Reduce Rust-side per-tx overhead (storage map building, cloning, FFI)
  (b) Reduce sequential phase overhead (preParallelApply, commit, finalize)
  (c) Increase parallelism (more threads, batch processing)
- The gap between current 9,408 TPS and theoretical 4-thread max of ~15,267 TPS
  is ~60% from sequential phases and ~40% from per-tx C++ overhead. Neither is
  large enough individually for a single optimization to break 10K TPS.

## Conclusion
Reverted. Directionally correct but marginal impact (+1.4%). The optimization
saves only ~3-5µs/tx which is too small relative to the ~262µs total per-tx
time. Future experiments should target larger structural changes.
