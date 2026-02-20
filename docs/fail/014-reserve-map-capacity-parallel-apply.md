# Experiment 014: Pre-Reserve Parallel Apply Containers

## Date
2026-02-20

## Hypothesis
Repeated rehashing/allocation in hot unordered containers during parallel apply
adds avoidable overhead. Pre-reserving container capacity for stage/global/thread
maps and sets should reduce allocator churn and improve apply-path throughput.

## Change Summary
Implemented one focused optimization in `src/transactions/ParallelApplyUtils.cpp`:
- Added `reserve()` for the RO TTL set in `buildRoTTLSet`.
- Added `reserve()` for stage read-write key collection in `getReadWriteKeysForStage`.
- Added `reserve()` for `mGlobalEntryMap` in
  `preParallelApplyAndCollectModifiedClassicEntries` using an upper-bound
  estimate from footprints.
- Added `reserve()` for `mThreadEntryMap` in
  `collectClusterFootprintEntriesFromGlobal` using an upper-bound estimate from
  cluster footprints.

## Results

### TPS
- Baseline: 14,144 TPS (`docs/success/011-indirect-bucket-sort.md`)
- Post-change: Not measured
- Delta: Not measured

### Build/Test Gate
- Build: `make -j$(nproc)` succeeded (with `CCACHE_DISABLE=1` due sandbox ccache
  temp/cache permission issues).
- Required test command executed:
  `env NUM_PARTITIONS=20 TEST_SPEC="[tx]" make check`
- Result: failed before benchmark gate due sandbox/runtime restriction, with
  repeated failures such as:
  - `open: Operation not permitted`
  - missing `lldb` in rerun path (`run-selftest-nopg`)

### Tracy Analysis
No new benchmark/trace captured, per hard rule: do not run benchmark if unit
tests do not pass.

Latest available baseline trace reviewed in this iteration:
`/mnt/xvdf/tracy/exp012-confirm.tracy`
- Dominant self-time under apply path remains
  `applySorobanStageClustersInParallel`.
- `finalizeLedgerTxnChanges` remains a secondary hotspot.

## Why It Failed
This experiment was blocked at the mandatory unit-test gate in the current
sandbox environment. Because tests did not pass, benchmarking was intentionally
not run to comply with hard constraints. Without post-change benchmark data,
this optimization cannot be validated for TPS impact in this iteration.

## Files Changed
- `src/transactions/ParallelApplyUtils.cpp` (reverted after failed gate)
- `docs/fail/014-reserve-map-capacity-parallel-apply.md`
