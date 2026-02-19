# Experiment 006: Remove Tracy Spans from Hottest Rust Functions

## Status: MARGINAL / NOT WORTH PURSUING

## Hypothesis
Tracy `tracy_span!` calls in ultra-hot Rust functions add measurable overhead
even when no profiler is connected (`ondemand` mode), due to FFI calls into C++
Tracy code (atomic loads + branch checks). Removing spans from the 6 hottest
functions should save ~1.0-1.1s across 7 ledgers = ~157ms/ledger = ~12.7%.

Targeted spans (by call count in 30s trace):
1. `charge` — 51.5M calls × 4 FFI (begin+text+value+end)
2. `visit host object` — 6.7M calls × 2 FFI
3. `map lookup` — 2.7M calls × 2 FFI
4. `new map` — 513K calls × 2 FFI
5. `write xdr` — 513K calls × 2 FFI
6. `new vec` — 628K calls × 2 FFI

## Implementation
1. Removed `tracy_span!("charge")` block (including `emit_text`/`emit_value`) from
   `dimension.rs:174-179` in p25 soroban-env-host
2. Removed `tracy_span!("visit host object")` from `host_object.rs:468`
3. Removed `tracy_span!("map lookup")` from `metered_map.rs:173`
4. Removed `tracy_span!("new map")` from `metered_map.rs:148`
5. Removed `tracy_span!("write xdr")` from `metered_xdr.rs:61`
6. Removed `tracy_span!("new vec")` from `metered_vector.rs:107`
7. Kept coarse-grained spans (`invoke_host_function`, `SAC transfer`, etc.)

## Results
- **Baseline**: 9,408 TPS (exp002)
- **Experiment**: 9,536 TPS
- **Change**: +1.4% (+128 TPS)
- **Tracy trace**: `/mnt/xvdf/tracy/exp006-tracy-spans.tracy`

## Tracy Self-Time Comparison (charge zone)
- exp002: 2.316s self-time across 7 ledgers (51.5M calls)
- exp006: 1.909s self-time across 6 ledgers (52.1M calls)
- Reduction: ~407ms in raw self-time, but different ledger counts

## Why the Impact Was Much Smaller Than Estimated
1. **FFI overhead per call is ~1-2ns, not ~5ns**: The `ondemand` path does a single
   atomic load (`CLIENT_STATE`) + branch-not-taken. Modern CPUs execute this in
   ~1-2ns, making the total overhead from 51.5M charge calls ~50-100ms, not ~1s.

2. **Zone name resolution is cached**: Tracy's `span!` macro uses `once_cell::Lazy`
   for the `SpanLocation` static, so the string/location setup is only done once
   per zone. Subsequent calls only do the atomic check.

3. **Branch predictor handles it well**: Since the profiler is never connected during
   benchmarks, the `is_running()` branch is always false. The branch predictor
   learns this quickly and the misprediction rate drops to near zero.

4. **Submodule complexity**: Changes are in the p25 git submodule, making
   commit/push difficult without upstream coordination.

## Key Learning
- Tracy `ondemand` overhead per span is ~1-2ns (atomic load + predicted branch),
  not the ~5ns estimated. Even with 51.5M calls, total overhead is only ~50-100ms
  across a 30s trace — not enough to significantly move TPS.
- The `charge` span's extra `emit_text()` + `emit_value()` calls are gated behind
  the same `is_running()` check, so they add zero overhead when profiler is not
  connected.
- Micro-optimizations targeting <2% gains are generally not worth the maintenance
  cost, especially in submodule code.

## Conclusion
Reverted. The optimization is directionally correct but the impact (+1.4%) is
within noise range and not worth the submodule maintenance burden. Future
optimization efforts should focus on algorithmic changes (reducing work per
transaction) rather than micro-optimizations to profiling infrastructure.
