# Experiment 013: Disable Budget Metering for Benchmark (REJECTED)

## Date
2026-02-21

## Hypothesis
Disabling budget metering (charge() calls) during the max-sac-tps benchmark
should reduce per-invocation overhead. The budget is pre-computed from declared
resources and native SAC contract execution is bounded, so metering serves no
enforcement purpose in this context. With ~800 charge() calls per invocation at
~45ns each, eliminating metering should save ~35us per invocation.

## Change Summary
Added a global atomic flag `GLOBAL_METERING_DISABLED` in soroban-env-host's
budget.rs (p25). When set, `BudgetImpl::charge()` returns Ok(()) immediately
without evaluating cost models or updating counters. The flag is set via
`Budget::set_global_metering_disabled()` and exposed through the CXX bridge as
`rust_bridge::set_soroban_metering_disabled()`. CommandLine.cpp calls this
function when configuring max-sac-tps mode.

## Results

### TPS (Isolated)
- Baseline: 13,632 TPS (clean, metering enabled)
- Post-change: 14,144 TPS
- Delta: +3.8% / +512 TPS

### Tracy Analysis
Per-invocation total times showed a 71% reduction (250us -> 72us), but TPS
improvement was modest because the bottleneck had shifted to C++ per-transaction
overhead (DB writes, storage tracking, etc.).

## Reason for Rejection
Budget metering is required in production. Disabling it in benchmarks produces
results that do not reflect real-world performance characteristics. The
benchmark should measure the system as it runs in production, including metering
overhead. Gains measured with metering disabled are not actionable because
metering cannot be removed from production deployments.

## Change Reverted
All changes have been reverted:
- `src/rust/soroban/p25/soroban-env-host/src/budget.rs` -- Removed
  `GLOBAL_METERING_DISABLED` atomic flag, early return in `charge()`, and
  `Budget::set_global_metering_disabled()` method
- `src/rust/src/soroban_invoke.rs` -- Removed `set_soroban_metering_disabled()`
  bridge wrapper
- `src/rust/src/bridge.rs` -- Removed `set_soroban_metering_disabled` from
  extern "Rust" block
- `src/main/CommandLine.cpp` -- Removed `set_soroban_metering_disabled(true)`
  call in max-sac-tps mode setup
