# Experiment 016: Indirect Sort in convertToBucketEntry

## Date
2026-02-20

## Hypothesis
`convertToBucketEntry` sorts a `vector<BucketEntry>` where each element is
200-500 bytes (containing full XDR `LedgerEntry` payloads). `std::sort` swaps
these large objects during partitioning, which is expensive due to memory
copies. By sorting lightweight 24-byte reference structs (`EntryRef`: type tag
+ pointer) and materializing the final `BucketEntry` vector in one sequential
pass, we can reduce sort time significantly. This function costs 32ms/ledger
on the critical path inside `addLiveBatch`, which itself runs in parallel with
`updateInMemorySorobanState` but gates the overall `finalizeLedgerTxnChanges`
completion.

## Change Summary
Rewrote `LiveBucket::convertToBucketEntry` to use indirect sorting:

1. **Define `EntryRef` struct** (24 bytes): `BucketEntryType` tag + pointer
   to source `LedgerEntry` (for INIT/LIVEENTRY) or `LedgerKey` (for DEADENTRY).

2. **Build `vector<EntryRef>`** by iterating init, live, and dead input vectors,
   storing pointers back to the original entries (no copies).

3. **Sort the refs** using the same `LedgerEntryIdCmp` comparison logic but
   operating through pointers. Swaps move 24 bytes instead of 200-500 bytes.

4. **Materialize `vector<BucketEntry>`** in one sequential pass over the sorted
   refs, copying each entry exactly once into its final position.

5. **Retain debug assertion** (`#ifndef NDEBUG`) verifying sort order using
   `BucketEntryIdCmp`.

## Results

### TPS
- Baseline: 13,760 TPS (experiment 015)
- Post-change: 14,144 TPS [14,144 - 14,208]
- Delta: **+384 TPS (+2.8%)**

### Tracy Analysis (exp015 baseline vs exp016)

| Zone | exp015 mean (ms) | exp016 mean (ms) | Delta |
|------|-------------------|-------------------|-------|
| convertToBucketEntry | 31.9 | 25.4 | **−20.5%** |
| freshInMemoryOnly | 32.0 | 25.5 | **−20.3%** |
| addLiveBatch | 83.3 | 77.0 | **−7.5%** |
| applyLedger | 1,343 | 1,332 | **−0.8%** |

The `convertToBucketEntry` zone dropped by 6.5ms/ledger (20.5%), which
propagated through `freshInMemoryOnly` and `addLiveBatch`. The `applyLedger`
improvement is modest (11ms, 0.8%) because `addLiveBatch` runs in parallel
with `updateInMemorySorobanState` — the savings only help when `addLiveBatch`
is the longer of the two parallel tasks.

## Why It Worked
The original code sorted `vector<BucketEntry>` objects in-place. Each swap
during `std::sort` moved ~300 bytes on average (XDR-serialized ledger entries).
With ~14,000 entries per ledger and O(n log n) comparisons/swaps, the sort
performed ~200K swaps of large objects.

The indirect approach:
- **Sort phase**: swaps 24-byte `EntryRef` structs (12.5x smaller), improving
  cache utilization and reducing memcpy overhead
- **Materialize phase**: copies each entry exactly once into its final sorted
  position (sequential access pattern, cache-friendly)
- **Net effect**: same comparison count but dramatically cheaper swap operations

## Files Changed
- `src/bucket/LiveBucket.cpp` — rewrote `convertToBucketEntry` with indirect sort

## Commit
