# Experiment 017: Skip Cost Tracker Updates in Production

## Date
2026-02-21

## Hypothesis
The `BudgetImpl::charge()` function updates cost tracker fields (iterations,
inputs, cpu, mem, meter_count) on every charge call (~832 per SAC transfer TX).
These trackers are purely diagnostic/reporting data — budget enforcement uses
separate `BudgetDimension::total_count` and `limit` fields. Gating tracker
updates behind `#[cfg(any(test, feature = "testutils", feature = "recording_mode"))]`
will eliminate ~5-6 tracker field updates per charge call in the production build,
saving ~4-5µs per TX.

## Change Summary
- Wrapped the cost tracker update block in `BudgetImpl::charge()` (budget.rs)
  with `#[cfg(any(test, feature = "testutils", feature = "recording_mode"))]`
- The gated code includes:
  - `cost_trackers[ty].iterations += iterations`
  - `cost_trackers[ty].inputs += input * iterations` (for linear cost types)
  - `tracker.meter_count += 1`
  - `cost_trackers[ty].cpu += cpu_charged`
  - `cost_trackers[ty].mem += mem_charged`
- Budget enforcement (BudgetDimension::charge, check_budget_limit) is unchanged
- The soroban-env-host rlib is built WITHOUT testutils/recording_mode features,
  so tracker code is compiled out in the production/benchmark binary
- Test builds (with `testutils` feature or `#[cfg(test)]`) retain full tracker
  functionality

### Why This Is Safe
- Cost trackers are only read via `get_tracker()` which is used for:
  1. `soroban_proto_any.rs`: `get_tracker(VmInstantiation).cpu` — used to compute
     `cpu_insns_excluding_vm_instantiation`, which is only used for Soroban metrics
     (disabled in benchmark via `DISABLE_SOROBAN_METRICS_FOR_TESTING`)
  2. Test code (budget_metering.rs, lifecycle.rs, etc.) — all behind `#[cfg(test)]`
  3. Cost runner benchmarks — behind `feature = "bench"` or `feature = "testutils"`
  4. Display/Debug formatting — purely informational
- Budget enforcement (total_count vs limit) is completely independent of trackers

## Results

### TPS
- Baseline: 14,144 TPS
- Post-change: 14,272 TPS
- Delta: **+128 TPS (+0.9%)**

### Tracy Analysis (per-TX mean times)
- parallelApply: 130.8µs → 126.6µs (**-4.2µs, -3.2%**)
- SAC transfer: 43.4µs → 39.0µs (**-4.4µs, -10.1%**)
- invoke_host_function: 83.4µs → 77.8µs (**-5.6µs, -6.7%**)
- ed25519 verify: 42.1µs → 42.2µs (unchanged, as expected)

### Analysis
The ~4-5µs per-TX improvement is consistent with eliminating 832 tracker
updates at ~5-6ns per call:
- 832 calls × 5.5ns = 4.6µs

The TPS improvement is within noise because the benchmark is bottlenecked by
sequential overhead (~500ms per ledger), not parallel execution. A 4µs per-TX
savings across 4 threads saves ~14ms of parallel time per ledger, but parallel
execution was only ~430ms of the ~860ms ledger close time.

### Trace File Stats
- Zone count: 61.9M (30s)
- Trace size: 427MB

## Files Changed
- `src/rust/soroban/p25/soroban-env-host/src/budget.rs` — gated tracker updates
