# Experiment 066: Replace InMemoryBucketEntry Virtual Set with unordered_map

## Date
2026-02-24

## Hypothesis
`InMemoryBucketState` used an `unordered_set<InternalInMemoryBucketEntry>` where
every `scan()` call (705K calls in a 30s trace) required:
1. Heap-allocating a `QueryKey` via `std::make_unique`
2. Virtual dispatch for `hash()` and `operator==` through `AbstractEntry`
3. Heap-deallocating the `QueryKey` after lookup

Replacing this with `std::unordered_map<LedgerKey, IndexPtrT>` eliminates all
per-lookup heap allocation and virtual dispatch. The LedgerKey is stored
separately from the BucketEntry (slightly more memory at construction time)
but lookups become a direct `unordered_map::find()` with no heap allocation
or virtual dispatch.

## Change Summary
Removed the entire `InternalInMemoryBucketEntry` class hierarchy (~120 lines)
including `AbstractEntry`, `ValueEntry`, `QueryKey`, and
`InternalInMemoryBucketEntryHash`. Replaced `unordered_set` with
`unordered_map<LedgerKey, IndexPtrT>`.

- `insert()`: Extracts key via `getBucketLedgerKey()`, emplaces key+value pair
- `scan()`: Direct `mEntries.find(searchKey)` — no heap allocation, no vtable
- `operator==` (BUILD_TESTS only): Moved to .cpp, compares map entries by key
  lookup and value comparison using `!(a == b)` pattern (XDR types lack `!=`)

## Results

### TPS
- Baseline: 19,136 TPS (interval [299, 300])
- Post-change: 19,520 TPS (interval [305, 307])
- Delta: +384 TPS (+2.0%)

### Analysis
The improvement comes from eliminating ~705K heap allocations per 30s trace
(~23K per ledger) in the `scan()` hot path. Each allocation/deallocation cycle
for `QueryKey` involved `make_unique` + virtual dispatch overhead.

## Files Changed
- `src/bucket/InMemoryIndex.h` — Removed `InternalInMemoryBucketEntry` class
  hierarchy (~120 lines). Changed `InMemoryBucketState` to use
  `unordered_map<LedgerKey, IndexPtrT>`. Moved `operator==` declaration to
  non-inline.
- `src/bucket/InMemoryIndex.cpp` — Updated `insert()` for map emplacement,
  `scan()` for direct map lookup, added `operator==` implementation comparing
  map entries by key lookup and `BucketEntry` value equality.

## Commit
<to be filled after commit>
