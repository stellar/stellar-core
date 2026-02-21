# Experiment 016: Remove Tracy Zone from Budget Charge

## Date
2026-02-21

## Hypothesis
The `BudgetDimension::charge` function has a Tracy zone (`tracy_span!("charge")`)
that fires 832 times per SAC transfer transaction. Each zone call also emits
`emit_text` (cost type name) and `emit_value` (amount). With Tracy enabled,
this creates ~55.7M zone events per 30-second trace, inflating measured per-TX
times from ~131us to ~277us. Removing these zones will improve profiling
accuracy and may improve TPS by eliminating zone overhead.

## Change Summary
- Removed `#[cfg(all(not(target_family = "wasm"), feature = "tracy"))]` Tracy
  zone block from `BudgetDimension::charge()` in all protocol versions (p22-p25).
  p21 does not have Tracy support.
- The removed block contained:
  ```rust
  if _is_cpu.0 {
      let _span = tracy_span!("charge");
      _span.emit_text(ty.name());
      _span.emit_value(amount);
  }
  ```

## Build System Discovery
Removing the Tracy zone from p25 only was insufficient because:
1. Cargo caches the `stellar-core` Rust crate independently of the soroban rlibs
2. Deleting the soroban-libs.stamp and individual rlib fingerprints is necessary,
   BUT you must also delete the stellar-core Rust crate fingerprints at
   `target/release/.fingerprint/stellar-core-*` to force a full relink
3. Without cleaning the stellar-core fingerprints, the old compiled objects
   (which still reference the charge LOC symbols) persist in
   `librust_stellar_core.a`

## Results

### TPS
- Baseline: 14,144 TPS
- Post-change: 14,144 TPS
- Delta: **0%** (no change)

### Tracy Analysis
- Zone count per 30s trace: 171M -> 55.4M (**-67.6%**)
- Trace file size: 966MB -> 380MB (**-60.7%**)
- parallelApply reported time: 277us/TX -> 131us/TX (**-52.7%**)
- Host::invoke_function reported time: 151us/TX -> 51us/TX (**-66.2%**)
- SAC transfer reported time: ~15us -> ~43us self-time (now accurately measured)
- charge function was completely inlined by the compiler after zone removal

### Why TPS Didn't Change
The Tracy ring buffer uses lock-free writes that take only ~10-15ns per event.
The 832 charge zones per TX add ~40us of actual CPU overhead, but this is within
the measurement noise. The 277us -> 131us reduction in Tracy-reported times is
due to *measurement distortion*: each zone's begin/end requires an rdtsc call,
and the profiler charges this time to the parent zone, inflating parent times.
The actual CPU execution time was always ~131us; Tracy just couldn't measure it
accurately with 832 nested zones per TX.

## Value of This Change
Despite no TPS improvement, this change is retained because:
1. Tracy profiles are 60-67% smaller and faster to analyze
2. Per-TX measurements are now accurate (131us real vs 277us inflated)
3. The "charge" zone with 832 calls/TX was excessive instrumentation that
   obscured actual hotspots
4. Future optimization work benefits from accurate baseline measurements
5. The compiler can now inline BudgetDimension::charge (all 4 text symbols
   disappeared from the binary, reducing binary size by ~388KB)

## Key Discovery: Real Per-TX Breakdown
With accurate Tracy measurements, the actual per-TX time breakdown is:
- parallelApply: 131us (was reported as 277us)
- Host::invoke_function: 51us
- SAC transfer: 43us
- ed25519 verify: 47us
- Sequential overhead: ~540ms per ledger (the real bottleneck)

The benchmark is bottlenecked by sequential work (commit, DB writes, bucket
operations), NOT by parallelApply. Future optimizations should target the
sequential path.

## Files Changed
- `src/rust/soroban/p22/soroban-env-host/src/budget/dimension.rs`
- `src/rust/soroban/p23/soroban-env-host/src/budget/dimension.rs`
- `src/rust/soroban/p24/soroban-env-host/src/budget/dimension.rs`
- `src/rust/soroban/p25/soroban-env-host/src/budget/dimension.rs`
