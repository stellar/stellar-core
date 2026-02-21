# Experiment 019: Bypass RefCell Borrow Check in Budget::charge

## Date
2026-02-21

## Hypothesis
`Budget::charge()` calls `self.0.try_borrow_mut_or_err()?.charge(ty, 1, input)`
832 times per SAC transfer TX. Each call does a RefCell borrow check (read
borrow flag, compare, write flag, create RefMut guard, on drop restore flag).
Since the Budget is only accessed from a single thread during Soroban invocation
with no recursive borrows, we can bypass the RefCell borrow checking using
`RefCell::as_ptr()` to get a raw pointer, eliminating ~3-5ns of overhead per call.

## Change Summary
- Modified `Budget::charge()` in budget.rs to use `unsafe { &mut *self.0.as_ptr() }`
  instead of `self.0.try_borrow_mut_or_err()?` in production builds
- Gated behind `#[cfg(not(any(test, feature = "testutils")))]` so test builds
  retain RefCell safety checking
- The soroban-env-host rlib is built without testutils, so the unsafe path is
  used in the benchmark/production binary

### Safety Argument
- Budget is wrapped in `Rc<RefCell<BudgetImpl>>` — single-threaded only (Rc)
- No recursive borrows: charge() calls BudgetImpl::charge() which does not
  call Budget::charge() again
- Each parallel execution thread has its own Host/Budget instance
- Test builds retain full RefCell checking via the cfg gate

## Results

### TPS
- Baseline (exp-017): 14,272 TPS
- Post-change: 14,144 TPS (within noise — binary search oscillates between these)
- Delta: within noise

### Tracy Analysis (per-TX mean times)
- parallelApply: 126.6µs → 124.9µs (**-1.7µs, -1.3%**)
- SAC transfer: 39.0µs → 38.6µs (**-0.4µs, -1.0%**)
- invoke_host_function: 77.8µs → 76.3µs (**-1.4µs, -1.8%**)
- ed25519 verify: unchanged (as expected)

### Cumulative Results (from exp-016e baseline)
- parallelApply: 130.8µs → 124.9µs (**-5.9µs, -4.5%**)
- SAC transfer: 43.4µs → 38.6µs (**-4.8µs, -11.0%**)
- invoke_host_function: 83.4µs → 76.3µs (**-7.1µs, -8.5%**)

### Analysis
The ~1.7µs savings is consistent with eliminating RefCell overhead:
- 832 calls × ~2ns saved per call = 1.7µs

The actual savings per call (~2ns) is lower than the estimated 5-12ns because:
1. The borrow flag was in L1 cache (frequently accessed)
2. The compiler already partially optimized the RefCell operations
3. RefMut guard is a zero-cost abstraction when inlined

TPS didn't change because the bottleneck is sequential overhead (~500ms per
ledger), not per-TX parallel execution time.

## Files Changed
- `src/rust/soroban/p25/soroban-env-host/src/budget.rs` — bypass RefCell in charge()
