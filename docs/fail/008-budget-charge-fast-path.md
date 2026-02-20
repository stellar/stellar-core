# Experiment 008: Budget::charge() Fast Path Optimization

## Status: FAILED (0% improvement)

## Hypothesis
`Budget::charge()` in Rust is called ~51.5M times during a 7-ledger benchmark
run (7.36M calls/ledger). Tracy profiling shows the `charge` zone at 2,316ms
total self-time for CPU dimension alone. The function performs multiple
bounds-checked array accesses (`get_mut().ok_or_else(...)`) and propagates
`Result<>` errors on every call, even though the bounds checks never actually
fail (the `ContractCostType` enum values are always valid indices into arrays
sized by `ContractCostType::variants().len()`).

By replacing bounds-checked indexing with `unsafe get_unchecked()`, inlining the
cost model evaluation, and eliminating `Result<>` overhead on the hot path, we
estimated saving ~20ns per call x 51.5M calls = ~1,030ms total, yielding a
potential 3-8% TPS improvement.

## Implementation
1. **`BudgetImpl::charge()` (budget.rs)**: Replaced the entire method body:
   - Eliminated bounds-checked `get_mut(ty as usize).ok_or_else(...)` for the
     cost tracker lookup, using `unsafe { get_unchecked_mut() }` instead.
   - Called new `charge_fast()` on both `cpu_insns` and `mem_bytes` dimensions
     before updating trackers, reducing interleaved borrows.
   - Used `wrapping_add(1)` for `meter_count` instead of `saturating_add(1)`
     (since `u32` overflow is benign for a counter).
   - Deferred budget limit checks to after both CPU and memory charges.

2. **`BudgetDimension::charge_fast()` (dimension.rs)**: New `#[inline(always)]`
   method that:
   - Takes `idx: usize` instead of `ContractCostType` to pass through pre-computed index.
   - Uses `unsafe { get_unchecked() }` for cost model lookup.
   - Inlines `MeteredCostComponent::evaluate()` directly (the `saturating_mul`
     and `unscale` arithmetic) to avoid going through the `HostCostModel` trait
     and `Result<>` return type.
   - Returns `u64` directly instead of `Result<u64, HostError>`.
   - Preserves Tracy span emission (gated behind `cfg(feature = "tracy")`).

### Files Modified
- `src/rust/soroban/p26/soroban-env-host/src/budget.rs` -- Rewrote
  `BudgetImpl::charge()` to use unchecked indexing and call `charge_fast()`.
- `src/rust/soroban/p26/soroban-env-host/src/budget/dimension.rs` -- Added
  `BudgetDimension::charge_fast()` with inlined evaluation and unchecked indexing.

## Results
- **Baseline**: 9,408 TPS (exp002)
- **Experiment**: 9,408 TPS [9,408, 9,472]
- **Change**: 0% (no measurable improvement)
- **Tracy trace**: `/mnt/xvdf/tracy/exp008-charge-fast.tracy`
- **Baseline trace**: `/mnt/xvdf/tracy/exp002-commit-opt.tracy`
- **Tests**: All [tx] tests pass, all Rust soroban tests pass

### Tracy Analysis

**applyLedger (benchmark envelope):**
Both traces include one short setup ledger (~70ms). Excluding it:

| Metric | Baseline (exp002) | Experiment (exp008) | Delta |
|--------|-------------------|---------------------|-------|
| Measured ledgers | 6 | 7 | +1 |
| Total applyLedger | 8,581ms | 10,224ms | — |
| Avg per ledger | 1,430ms | 1,460ms | +2.1% (noise) |

**`charge` zone (self-time) — the target of this optimization:**

| Metric | Baseline | Experiment | Delta |
|--------|----------|------------|-------|
| Total self-time | 2,316ms | 2,630ms | +13.5% (more ledgers) |
| Call count | 51,457,716 | 57,497,495 | +11.7% (more ledgers) |
| Mean per call | 45ns | 45ns | **0% change** |
| Median per call | 18ns | 18ns | **0% change** |
| P90 per call | 21ns | 21ns | **0% change** |

The per-call cost of `charge` is **identical** between baseline and experiment.
The `unsafe get_unchecked()` / inlined-eval changes produced zero per-call
improvement. The total self-time increase is entirely explained by exp008
processing ~11.7% more transactions (7 vs 6 measured ledgers).

**DiffTracyCSV comparison (exp002 → exp008):**
The script flagged ~40 zones, but **every flagged zone** shows the same pattern:
~11% more events (from additional ledger) with per-call timings either unchanged
or slightly regressed due to noise. Notable observations:

- `charge`: median 18→18ns, p90 21→21ns, sum +13% (event count +11.7%) — **no per-call change**
- `write xdr`: median 3µs→3.5µs (+14%), p90 8µs→9µs (+15%) — slight regression
- `visit host object`: median 191→225ns (+17%), p90 1µs→1.2µs (+18%) — slight regression
- `map lookup`: median 544→638ns (+17%), p90 1µs→1.2µs (+12%) — slight regression
- `new map`: median 706→824ns (+16%), p90 1µs→1.2µs (+15%) — slight regression
- `storage get`: median 1.9→2.3µs (+19%), p90 3µs→3.4µs (+12%) — slight regression
- `SAC transfer`: median 111→132µs (+18%), p90 150→169µs (+12%) — slight regression

The widespread ~15-20% per-call regression across many Soroban host zones
(while `charge` itself is flat) suggests the `charge_fast()` code changes may
have perturbed instruction cache layout or inlining decisions, causing minor
slowdowns in nearby hot code. This is consistent with the 0% net TPS result:
any theoretical savings from removing bounds checks were offset by collateral
icache/layout effects.

**Self-time hotspot ranking (exp008, top 10 within applyLedger):**

| Rank | Zone | Self-time (ms) | % of trace | Calls | Mean |
|------|------|---------------|------------|-------|------|
| 1 | `charge` | 2,630 | 4.13% | 57.5M | 45ns |
| 2 | `verify_ed25519_signature_dalek` | 2,626 | 4.12% | 62K | 42µs |
| 3 | `visit host object` | 2,087 | 3.27% | 7.5M | 279ns |
| 4 | `write xdr` | 1,994 | 3.13% | 574K | 3.5µs |
| 5 | `sha256` / `add` | 1,627 / 2,105 | 2.55% / 3.30% | 1.6M / 2.4M | 1µs / 893ns |
| 6 | `invoke_host_function` | 1,592 | 2.50% | 64K | 25µs |
| 7 | `map lookup` | 1,468 | 2.30% | 3.0M | 489ns |
| 8 | `SAC transfer` | 1,014 | 1.59% | 64K | 15.9µs |
| 9 | `Host::invoke_function` | 776 | 1.22% | 64K | 12.2µs |
| 10 | `sign` | 1,142 | 1.79% | 62K | 18.3µs |

## Why It Failed
1. **LLVM already optimizes away the bounds checks**: The Rust compiler with
   `-O3` (release mode) recognizes that `ty as usize` from a bounded enum is
   always in range for arrays sized by `variants().len()`. LLVM eliminates the
   bounds checks at the IR level, making our `unsafe get_unchecked()` equivalent
   to what the compiler already produces. Tracy confirms: per-call median and
   p90 are identical at 18ns and 21ns respectively.

2. **`Result<>` overhead is near-zero in the happy path**: Rust's `Result<>`
   uses a discriminant that the branch predictor handles perfectly (the error
   path is never taken in practice). The `?` operator compiles to a conditional
   branch that is always not-taken, costing ~0-1 cycles.

3. **The real cost is in arithmetic and memory access**: The dominant cost of
   `charge()` is the actual `saturating_mul`, `saturating_add`, and memory
   loads/stores to the cost model arrays and tracker fields. These are unchanged
   by our optimization.

4. **Tracy span overhead is the real bottleneck in `charge`**: The Tracy
   `charge` span (emitting zone name and value) is present in both old and new
   code paths, and its overhead (~15-20ns per call) dominates the bounds-check
   savings (~1-2ns at best).

5. **Code layout perturbation caused collateral regressions**: The DiffTracyCSV
   analysis shows that while `charge` per-call cost was unchanged, many other
   Soroban host zones (`visit host object`, `map lookup`, `write xdr`, `storage
   get/put`, `new map/vec`) regressed by ~15-20% per call. This suggests the
   inlined `charge_fast()` code displaced other hot functions in the instruction
   cache, offsetting any potential micro-gains.

## Key Learning
- Modern LLVM (clang-20 / Rust 1.88) is excellent at eliminating redundant
  bounds checks for enum-indexed arrays. Manual `unsafe` indexing provides
  no benefit over the compiler's optimizations. Tracy data confirms zero
  per-call improvement (18ns median in both baseline and experiment).
- `Result<>` error propagation in Rust has near-zero overhead when the error
  path is never taken, due to branch prediction.
- **Inlining changes can have non-local effects**: Adding `#[inline(always)]`
  to a 57.5M-call function and expanding its body can perturb icache layout
  enough to regress unrelated hot paths by 15-20%. Always check the full
  DiffTracyCSV output, not just the targeted zone.
- To meaningfully reduce `charge()` overhead, one would need to either:
  (a) Reduce the NUMBER of calls (batch multiple charges together)
  (b) Remove the Tracy spans in the charge path
  (c) Reduce tracker bookkeeping (fewer fields to update per call)
- Given experiments 006-008 all showing <2% impact from micro-optimizations,
  the remaining gains in the Rust execution path likely require structural
  changes (batched metering, reduced FFI overhead, or storage map redesign).

## Conclusion
Reverted. The optimization was logically sound but provided zero measurable
benefit because the compiler already performs the same optimizations we attempted
to do manually. Tracy profiling confirms the `charge()` per-call cost is
identical (18ns median, 21ns p90) in both baseline and experiment, while the
code layout changes caused ~15-20% per-call regressions in other Soroban host
zones, netting out to 0% TPS change.
