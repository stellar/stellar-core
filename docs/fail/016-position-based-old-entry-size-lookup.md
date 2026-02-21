# Experiment 016: Position-based old entry size lookup

## Date
2026-02-21

## Hypothesis
In `get_ledger_changes`, each old entry is re-serialized via `metered_write_xdr`
just to compute its XDR byte size for rent calculations. Since the input entries
are already XDR-encoded, we can capture their sizes during input parsing in
`build_storage_map_from_xdr_ledger_entries` and pass them to `get_ledger_changes`
for O(1) lookup by position index.

## Change Summary
- Changed `build_storage_map_from_xdr_ledger_entries` to return an `InitEntrySizeVec`
  alongside the storage map and TTL map.
- Built `ptr_to_size: Vec<(*const LedgerKey, u32)>` during input parsing.
- Added an extra `storage_map.iter(budget)?` loop after construction to reorder
  sizes into the storage map's sorted key order.
- Changed `get_ledger_changes` to use position-based indexing (`entry_idx`)
  instead of re-serializing old entries.
- All size-tracking code was cfg-gated to production-only mode.

## Results

### TPS
- Baseline: 13,632 TPS
- Post-change (combined with encoded_key skip): 10,944 TPS
- Delta: -19.7% / -2,688 TPS (REGRESSION)

### Tracy Analysis
- Per-invocation time improved: 250us -> 209us (-16.4%) -- the optimization worked
  at the micro level.
- But overall ledger apply was slower. At x=172 (11,008 TPS), mean apply time
  was 1,062ms vs 947ms baseline.
- BucketList operations (InMemoryIndex, readOne, resolve) showed significant
  increases in the Tracy trace, possibly due to changed resource contention
  patterns.

## Why It Failed
Despite reducing per-invocation time, the optimization caused a net regression
in overall TPS. The most likely causes:

1. The extra `storage_map.iter(budget)?` call in `build_storage_map_from_xdr_ledger_entries`
   adds metered work to every invocation's initialization path.
2. The `ptr_to_size.iter().find()` linear scan for pointer matching adds overhead
   per entry.
3. The changed memory access patterns may worsen cache behavior or resource
   contention in the parallel apply path.
4. The savings from skipping 2 old_entry serializations (~2 write_xdr) were
   small enough to be overwhelmed by the overhead of the indexing machinery.

## Lesson
Micro-optimizations that reduce per-invocation time can still cause regressions
at the macro level. Always validate with the full benchmark, not just Tracy
per-call analysis. The overhead of tracking/bookkeeping can exceed the savings
from avoiding the tracked operation, especially when N is small (only 2 entries
per invocation in the SAC transfer benchmark).
