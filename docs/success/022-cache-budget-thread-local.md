# Experiment 022: Cache Budget via Thread-Local Storage

## Date
2026-02-21

## Hypothesis
`Budget::try_from_configs` is called for every transaction, but the cost params
(`ContractCostParams` for CPU and memory) are identical for all transactions in a
ledger. This function deserializes two `ContractCostParams` XDR blobs via
`non_metered_xdr_from_cxx_buf` and runs `BudgetDimension::try_from_config` loops
(~50 iterations × 2 dimensions) per call. By caching the Budget in thread-local
storage and resetting only the per-TX counters (limits, trackers), we can
eliminate this repeated deserialization and cost model construction.

## Change Summary
- Added `reset_for_new_tx(cpu_limit, mem_limit)` method to `Budget` in all
  protocol versions (p21-p26) that resets counters/trackers without
  reconstructing cost models
- Modified `soroban_proto_any.rs` to use thread-local `RefCell<Option<...>>`
  cache keyed on the raw cost param bytes
- On cache hit: calls `reset_for_new_tx` + clone (Rc clone, cheap)
- On cache miss: calls `try_from_configs` and stores in cache
- Thread-local scope means each worker thread (4 threads from
  `std::async(std::launch::async, ...)`) gets its own cache per stage

### Safety Argument
- Cost params are identical for all TXs in a ledger — they come from
  `LedgerInfo` which is set per-ledger
- `reset_for_new_tx` resets exactly the same fields that `try_from_configs`
  initializes (counters to 0, limits to provided values, tracker to default)
- Cost models (the expensive part) are deterministic for given cost params
- Thread-local storage eliminates any cross-thread sharing concerns
- Cache is keyed on raw bytes, so any protocol upgrade that changes params
  will correctly miss and rebuild

## Results

### TPS
- Baseline (exp-021): 14,528 TPS
- Post-change: 14,656 TPS
- Delta: **+128 TPS (+0.9%)** (within benchmark variance)

### Tracy Analysis (per-TX mean times)
- parallelApply: 121.3µs → 120.3µs (**-1.0µs, -0.8%**)
- invoke_host_function_or_maybe_panic self: 5.5µs → 1.8µs (**-3.7µs, -67%**)
- invoke_host_function (Rust) self: 13.9µs → 14.3µs (noise)
- addReads self: 4.7µs → 4.7µs (unchanged)
- recordStorageChanges self: 5.2µs → 5.4µs (unchanged)
- Host::invoke_function self: 4.6µs (new zone tracked)
- e2e_invoke::invoke_function self: 4.2µs (new zone tracked)

### Cumulative Results (from exp-016e baseline)
- parallelApply: 130.8µs → 120.3µs (**-10.5µs, -8.0%**)

### Analysis
The 67% reduction in `invoke_host_function_or_maybe_panic` self-time confirms
the Budget construction was a significant per-TX cost. The function previously
spent ~5.5µs deserializing cost params and building cost models; now it spends
~1.8µs on cache lookup, reset, and Rc clone. The overall parallelApply
improvement is modest due to variance in other zones, but the targeted
optimization is clearly effective.

## Files Changed
- `src/rust/soroban/p{21,22,23,24,25,26}/soroban-env-host/src/budget.rs` —
  added `reset_for_new_tx` method
- `src/rust/src/soroban_proto_any.rs` — thread-local Budget caching with
  cost-param-bytes keyed cache
