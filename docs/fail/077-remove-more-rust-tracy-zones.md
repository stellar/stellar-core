# Experiment 077: Remove More High-Frequency Rust Tracy Zones — FAILED

## Hypothesis

Building on experiment 075's success (removing 4 tracy_span! calls = +1.0%), removing 17 more high-frequency tracy_span! calls from hot-path functions should yield additional improvement. The targeted zones had combined call counts of ~7.5M+ per benchmark run.

## Changes

Removed `tracy_span!` from 17 locations across 8 files:

**metered_xdr.rs** (4 zones):
- `hash xdr` — used for hashing entries
- `read xdr` — 329K calls, 118ms self-time
- `write xdr` — 548K calls, 445ms self-time
- `read xdr with budget` — 438K calls, 402ms self-time

**storage.rs** (5 zones):
- `storage get` — 767K calls, 158ms self-time
- `storage put` — in hot write path
- `storage del` — delete operations
- `storage has` — existence checks
- `extend key` — TTL extension

**metered_map.rs** (1 zone):
- `new map` — 987K calls, 140ms self-time

**metered_vector.rs** (1 zone):
- `new vec` — vector allocation

**conversion.rs** (3 zones):
- `Val to ScVal` (2 sites) — 2.4M calls, 325ms self-time
- `ScVal to Val` — 1.97M calls, 217ms self-time

**auth.rs** (1 zone):
- `require auth` — 109K calls, 103ms self-time

**frame.rs** (2 zones):
- `push context` — context frame management
- `pop context` — context frame management

**comparison.rs** (1 zone):
- `Compare<HostObject>` — host object comparison

## Results

- **Tests**: All 66 passed (49,011 assertions)
- **Run 1**: 18,944 TPS [18,944, 19,072] (baseline: 19,712)
- **Run 2**: 18,944 TPS [18,944, 19,072]
- **Verdict**: No improvement. Consistently at baseline noise floor.

## Analysis

Unlike experiment 075, this batch of tracy_span! removals did not show improvement. Possible reasons:

1. **Tracy short-circuits when disconnected**: When no Tracy server is connected (which is the case during the timed portion of the benchmark), `tracy_span!` performs only an atomic check of a flag and exits. The overhead is ~1-2ns per call, not the microseconds visible when profiling.

2. **Experiment 075 was possibly noise**: The 19,712 TPS measurement may have been an upward variance from the true mean of ~19,000 TPS, making the improvement appear real when it wasn't.

3. **Code layout effects**: Removing code (even unreachable branches) can change function layout, cache alignment, and inlining decisions. The net effect is unpredictable and can be slightly negative.

4. **Diminishing returns on Tracy removal**: The highest-frequency zones (visit_host_object at 7.87M, env function macro at 1M+) were already removed in experiment 075. These remaining zones, while numerous, have lower individual call counts.

## Conclusion

Bulk removal of Tracy zones across many hot-path functions does not yield measurable improvement. The Tracy instrumentation overhead, when disconnected, is negligible. Reverted all changes.
