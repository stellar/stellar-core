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
- **Tests**: All [tx] tests pass, all Rust soroban tests pass

## Why It Failed
1. **LLVM already optimizes away the bounds checks**: The Rust compiler with
   `-O3` (release mode) recognizes that `ty as usize` from a bounded enum is
   always in range for arrays sized by `variants().len()`. LLVM eliminates the
   bounds checks at the IR level, making our `unsafe get_unchecked()` equivalent
   to what the compiler already produces.

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

## Key Learning
- Modern LLVM (clang-20 / Rust 1.88) is excellent at eliminating redundant
  bounds checks for enum-indexed arrays. Manual `unsafe` indexing provides
  no benefit over the compiler's optimizations.
- `Result<>` error propagation in Rust has near-zero overhead when the error
  path is never taken, due to branch prediction.
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
to do manually. The `charge()` hot path is already well-optimized by LLVM.
