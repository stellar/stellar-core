# Experiment 018: Fast Charge Path (FAILED)

## Date
2026-02-21

## Hypothesis
Adding specialized `charge_single()` and `is_over_budget()` methods to
BudgetDimension that use direct array indexing (no bounds check), inline cost
evaluation (no Result wrapping), and skip the iterations parameter (always 1)
should reduce per-charge-call overhead and improve per-TX times.

## Change Summary
- Added `charge_single()` to BudgetDimension: direct `[ty as usize]` indexing,
  inline evaluate logic, returns u64 directly
- Added `is_over_budget()` to BudgetDimension: returns bool directly
- Added `if iterations == 1` branch in BudgetImpl::charge to use fast path

## Results

### TPS
- Baseline (exp-017): 14,272 TPS
- Post-change: 14,272 TPS
- Delta: 0% (unchanged)

### Tracy Analysis (per-TX mean)
- parallelApply: 126.6µs → 129.2µs (**+2.6µs, regression**)
- SAC transfer: 39.0µs → 41.2µs (+2.2µs)
- invoke_host_function: 77.8µs → 80.2µs (+2.4µs)

## Why It Failed
The optimization backfired because:
1. **The compiler was already optimizing**: With full inlining of
   BudgetDimension::charge (verified in exp-016), the compiler already
   eliminated bounds checks and Result wrapping through dead code elimination
   and constant propagation
2. **Code size increase**: Adding charge_single/is_over_budget plus the
   if-else branch increased code size, causing instruction cache pressure
3. **Branch prediction**: The `if iterations == 1` branch added an
   unnecessary prediction point
4. **Lesson**: Don't try to out-optimize the compiler's inliner. When
   functions are already fully inlined, manually duplicating their logic
   only increases code size without improving generated code quality.

## Change Reverted
All code changes reverted.
