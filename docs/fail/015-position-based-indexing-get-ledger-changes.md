# Experiment 015: Position-based Indexing in get_ledger_changes

## Date
2026-02-21

## Hypothesis
`get_ledger_changes` uses binary search (`init_storage_snapshot.get(key)` and
`footprint_map.get(key, budget)`) to look up each entry, but the storage map,
init storage map, and footprint all have the same keys in the same sorted order.
Using position-based O(1) indexing instead of O(log n) binary search should
eliminate ~2.3 µs per TX in get_ledger_changes.

## Change Summary
- Modified `get_ledger_changes` to accept `init_entries: &[(Rc<LedgerKey>, Option<EntryWithLiveUntil>)]`
- Replaced `init_storage_snapshot.get(key)` with `init_entries[i].1` position-based indexing
- Replaced `footprint_map.get(key, budget)` with `footprint_map.map[i].1` position-based indexing
- Used `storage.map.map.iter().enumerate()` instead of `storage.map.iter(budget)?`
- All changes gated with `#[cfg(not(any(test, feature = "recording_mode")))]`

## Results

### TPS
- Baseline: 14,144 TPS
- Post-change: 13,888 TPS
- Delta: **-1.8%** (slight regression)

### Tracy Analysis
- parallelApply mean: 277 µs → 285 µs (+3.1%, within noise)
- get_ledger_changes total: 27.5 µs → 27.9 µs (unchanged)
- Map lookup calls slightly increased (different TX count during capture)

## Why It Failed

1. The binary searches being eliminated operate on small maps (7-10 entries),
   where binary search does only ~3 comparisons. Each search saves ~130-200ns.
2. Total savings: 2 searches × 7 entries × ~165ns = ~2.3 µs per TX.
3. But get_ledger_changes is only 27.5 µs total, and the binary searches are
   a small fraction of that (most time is in XDR serialization of old entries).
4. 2.3 µs savings out of 277 µs parallelApply = 0.8%, well within noise.

## Note
This is the second time position-based indexing has been tried (experiment 012
included a variant that caused a 20% TPS regression). The approach is sound
algorithmically but the savings are too small to matter at current performance
levels.

## Files Changed
- `src/rust/soroban/p25/soroban-env-host/src/e2e_invoke.rs`
