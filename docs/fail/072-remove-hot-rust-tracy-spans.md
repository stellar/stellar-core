# Experiment 072: Remove Tracy Spans from Ultra-Hot Rust Functions

## Status: FAILED (-3.0%)

## Hypothesis
Removing `tracy_span!` from 14 ultra-hot Rust functions (called millions of times per
30s benchmark window) would eliminate ~920ms/30s of Tracy profiling overhead, yielding
~3.2% TPS improvement.

## Changes
Removed `tracy_span!` from 14 functions across 7 files:

1. **host_object.rs**: `add_host_object`, `visit_obj_untyped`
2. **metered_map.rs**: `from_exact_iter` ("new map"), `find` ("map lookup")
3. **metered_vector.rs**: `from_exact_iter` ("new vec")
4. **storage.rs**: `try_get_full_helper`, `put`, `del`, `has`
5. **conversion.rs**: `from_host_val`, `from_host_val_for_storage`, `to_host_val`
6. **comparison.rs**: `Compare<HostObject>::compare`
7. **vmcaller_env.rs**: Per-function tracy_span from Env trait dispatch macro

## Results
- **Baseline**: 19,520 TPS (experiment 070b)
- **Result**: 18,944 TPS [18,944 - 19,072]
- **Change**: -3.0% (regression)
- **Tracy zones**: 41.1M (down from ~51M baseline — confirming spans were removed)

## Analysis
The Tracy span removal was effective at reducing zone count (~10M fewer zones), but
the benchmark result was a regression rather than an improvement. Possible explanations:

1. **Code layout changes**: Removing the tracy_span! macro calls changes the generated
   machine code layout, affecting instruction cache behavior, branch prediction, or
   inlining decisions. The compiler may optimize differently without the tracy code
   present.

2. **Register pressure**: The tracy_span! macro may have been acting as an unintentional
   optimization barrier that prevented certain code transformations that happen to be
   harmful for this specific workload.

3. **Measurement noise**: The -3.0% is at the edge of measurement noise for this
   benchmark, though the binary search did converge with confidence.

## Conclusion
Tracy span overhead in hot Rust functions is not a significant bottleneck despite
the high call counts. The per-call overhead (~40-50ns) appears to be compensated by
favorable code generation effects. This approach should be abandoned as a line of
optimization.

## Tracy Profile
- `/mnt/xvdf/tracy/max-sac-tps-072.tracy`
