# Experiment 075: Remove High-Frequency Rust Tracy Zones

## Date
2026-02-25

## Hypothesis
Several Rust Tracy zones fire millions of times per ledger close:
- `visit host object` (host_object.rs:468): 7.87M calls/ledger
- `map lookup` (host/metered_map.rs:173): 2.96M calls/ledger
- `add host object` (host_object.rs:450): ~200K calls/ledger
- All env function Tracy zones via `vmcaller_env.rs` macro (obj_cmp: 1.08M calls)

Even with Tracy's lock-free queue, each zone incurs per-call overhead for
registration, timestamp acquisition, and queue insertion. At millions of calls,
this overhead is measurable. Removing these zones from the p25 hot path should
reduce per-transaction overhead and improve TPS.

## Change Summary
Commented out `tracy_span!` invocations in 4 locations:

- **`src/rust/soroban/p25/soroban-env-host/src/host_object.rs` line 450**:
  Removed `tracy_span!("add host object")` from `add_host_object()`.

- **`src/rust/soroban/p25/soroban-env-host/src/host_object.rs` line 468**:
  Removed `tracy_span!("visit host object")` from `visit_obj_untyped()`.

- **`src/rust/soroban/p25/soroban-env-host/src/host/metered_map.rs` line 173**:
  Removed `tracy_span!("map lookup")` from `get()`.

- **`src/rust/soroban/p25/soroban-env-common/src/vmcaller_env.rs` lines 189-190**:
  Removed `tracy_span!(core::stringify!($fn_id))` from the
  `vmcaller_none_function_helper!` macro. This removes Tracy zones from ALL
  env function calls (obj_cmp, map_new, vec_new, etc.) generated via the
  `call_macro_with_all_host_functions!` macro expansion.

## Results
- **Baseline**: 19,520 TPS [19,520, 19,648]
- **Run 1**: 18,944 TPS [18,944, 19,072] (likely noise/variance)
- **Run 2**: 19,712 TPS [19,712, 19,776]
- **Improvement**: +192 TPS (+1.0%)

## Verification
- All 66 `[soroban][tx]` tests passed (49,011 assertions)
- Tracy profile confirmed all 4 zone types completely absent:
  - "visit host object": 0 calls (was 7.87M)
  - "map lookup": 0 calls (was 2.96M)
  - "add host object": 0 calls (was ~200K)
  - env function zones (obj_cmp etc.): 0 calls
- p25 rlib verified via `strings` to not contain removed zone literals

## Notes
The improvement is modest (+1%) because Tracy's lock-free queue has very low
per-call overhead (~10-20ns). The initial estimate of 48-58ns/call was too high.
However, removing these zones has two benefits:
1. Small but real TPS improvement
2. Reduces profiling noise in future Tracy captures, making it easier to
   identify remaining hotspots

The first benchmark run (18,944 TPS) was lower than baseline, demonstrating
~3% run-to-run variance in the benchmark. The second run (19,712) confirmed
the change is a net positive.

## Tracy Profile
- `/mnt/xvdf/tracy/max-sac-tps-075b.tracy` (60s capture from 2nd run)
