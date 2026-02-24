# Experiment 014: Cache XDR Sizes in Vec to Skip Re-serialization

## Date
2026-02-21

## Hypothesis
`get_ledger_changes` serializes old entries to XDR just to get their byte
size for rent calculation. Since the XDR size is known at deserialization
time in `build_storage_map_from_xdr_ledger_entries`, caching the sizes
in a `Vec<(Rc<LedgerKey>, u32)>` and passing them to `get_ledger_changes`
should eliminate redundant serialization (~1.6 µs/TX savings).

## Change Summary
- Modified `build_storage_map_from_xdr_ledger_entries` to also return an
  `EntryXdrSizeVec` containing `(Rc<LedgerKey>, u32)` pairs
- Modified `get_ledger_changes` to accept an optional `&EntryXdrSizeVec`
  and look up sizes via linear scan instead of calling `metered_write_xdr`
- Gated Vec population with `#[cfg]` to skip in recording mode (to avoid
  changing budget consumption in tests)

## Results

### TPS
- Baseline: 14,144 TPS (consistent across 3 runs)
- Post-change: 13,888 TPS
- Delta: **-1.8%** (slight regression)

### Tracy Analysis
- `parallelApply` mean: 277 µs → 266 µs (-3.9%)
- `get_ledger_changes`: 27.5 µs → 25.9 µs (-6.1%)
- `build_storage_map`: 15.9 µs → 18.1 µs (+13.8%, overhead from Vec push + Rc clone)
- Net effect on those two zones: -1.6 µs + 2.2 µs = +0.6 µs worse

## Why It Failed

The implementation overhead exceeded the savings:
1. `Rc::clone()` for each key is not free (atomic refcount increment)
2. Linear scan of Vec for each entry in `get_ledger_changes` is O(n)
3. The actual XDR serialization being avoided was cheap (~1.6 µs for
   small entries like trustline/account)
4. `metered_write_xdr` budget charging was part of the cost, but the
   budget charges themselves add relatively little vs the atomic ops

To make this work, would need O(1) lookup without Rc cloning (e.g.,
index-based approach), but the savings are too small (~0.6% of 277 µs
path) to justify the complexity.

## Files Changed
- `src/rust/soroban/p25/soroban-env-host/src/e2e_invoke.rs`
