# Experiment 044: Optimize convertToBucketEntry Sort Comparator

## Date
2026-02-23

## Hypothesis
In `convertToBucketEntry`, sorting ~40K entries takes ~45ms/ledger due to
expensive `LedgerEntryIdCmp` comparisons (~75ns each). For SAC workloads where
all CONTRACT_DATA entries share the same contract address, ~50% of comparison
time is wasted on comparing the identical contract field (32-byte memcmp that
always returns "equal"). Additionally, each comparison dispatches through the
full XDR union comparison chain (4 levels of discriminant checks) before
reaching the actual discriminating bytes. By:

1. Caching `LedgerEntryType` in `EntryRef` to skip pointer dereferences for
   type dispatch
2. Detecting same-contract entries and skipping the 32-byte contract comparison
3. Inlining the SCV_ADDRESS key comparison to go directly to `memcmp`, bypassing
   the XDR union dispatch chain
4. Specializing TTL comparison to direct `keyHash` memcmp

We expected to reduce per-comparison cost from ~75ns to ~25-40ns, saving ~20-35ms
from the sort.

## Change Summary
Modified `LiveBucket::convertToBucketEntry` in `src/bucket/LiveBucket.cpp`:
- Added `entryType` field to `EntryRef` struct (cached from entry on construction)
- Added same-contract detection pass before sorting
- Rewrote sort comparator with fast paths for CONTRACT_DATA and TTL types
- CONTRACT_DATA fast path: skips contract comparison when same, inlines
  SCV_ADDRESS comparison to direct `std::memcmp` on 32-byte key data
- TTL fast path: direct `std::memcmp` on keyHash

## Results

### TPS
- Baseline: 16,640 TPS
- Post-change: 16,640 TPS [16,640, 16,768]
- Delta: **0%**

### Tracy Analysis
- `convertToBucketEntry` self-time: 37.5ms/ledger (was 45.4ms) — **-17%**
- `convertToBucketEntry` std dev: 0.73ms (was 13.6ms) — much more consistent
- `addLiveBatch`: 107ms/ledger (was 120ms) — **-11%**
- `finalizeLedgerTxnChanges`: 151ms (was 166ms) — **-9%**
- `applyLedger` total: 1,077ms (was 1,078ms) — NO meaningful change

## Why It Failed
The targeted zone (`convertToBucketEntry`) improved significantly, but
the savings don't propagate to the measured TPS because:

1. **Concurrency absorbs the savings**: `addLiveBatch` (which contains
   `convertToBucketEntry`) runs concurrently with `updateInMemorySorobanState`
   via `std::async`. The wall-clock time of `finalizeLedgerTxnChanges` is
   `max(addLiveBatch, updateInMemorySorobanState)`. With addLiveBatch at 107ms
   and updateState at 82ms, addLiveBatch is still the bottleneck, but the
   savings (120→107ms = 13ms) are only 1.2% of applyLedger.

2. **Cache misses dominate**: Even with the comparison optimizations, each
   comparison still dereferences two pointers to LedgerEntry objects scattered
   in heap memory. The cache miss cost (~25ns per dereference pair) is the
   dominant cost and cannot be reduced by optimizing the comparison logic alone.

3. **The sort is not on the critical path**: With 40K entries at ~37.5ms
   (optimized) out of a 1,078ms applyLedger, the sort contributes only 3.5%
   of the total time, and the concurrent execution further diminishes its impact.

### Key Insight
To make bucket sort improvements visible in TPS, either:
- `updateInMemorySorobanState` must also be optimized (to keep it off the
  critical path), OR
- The sort savings must be much larger (>40ms to bring addLiveBatch below
  updateState's 82ms), OR
- The sort must be eliminated entirely (pre-sorted insertion during commit)

## Files Changed (REVERTED)
- `src/bucket/LiveBucket.cpp` — optimized sort comparator in convertToBucketEntry
