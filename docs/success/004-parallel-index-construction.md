# Experiment 010: Parallelize InMemoryIndex Construction with Bucket Put Loop

## Date
2026-02-19

## Hypothesis
Inside `addLiveBatch` → `LiveBucket::mergeInMemory`, the put loop
(XDR serialize → SHA256 hash → disk write, ~80-90ms) and index construction
(`InMemoryIndex` from in-memory state, ~22ms) run sequentially but are
completely independent — both read `mergedEntries` as const. Running index
construction on a worker thread via `std::async` should save ~22ms per ledger
commit by fully overlapping it with the put loop.

## Change Summary
- `LiveBucket.cpp:mergeInMemory`: Launch `LiveBucketIndex` construction on
  async worker thread before the put loop. Collect the pre-built index with
  `indexFuture.get()` after the put loop completes.
- `BucketOutputIterator.h/.cpp:getBucket`: Added optional `preBuiltIndex`
  parameter. If provided, skip internal `LiveBucketIndex` construction.
  Existing-bucket index check still runs first for correctness.
- Added Tracy `ZoneNamedN` zones: `"mergeInMemory merge"`,
  `"mergeInMemory put loop"`, `"mergeInMemory index future wait"`.
- Added `#include <future>` to LiveBucket.cpp.

## Results

### TPS
- Baseline: 9,408 TPS
- Post-change: 9,408 TPS
- Delta: 0% (within binary search step granularity of 64 TPS)

### Tracy Micro-benchmark Analysis (30s capture, 7 ledger commits)

#### Key zone comparison (total time, mean per call)

| Zone | Baseline (mean/call) | Post-change (mean/call) | Delta |
|------|---------------------|------------------------|-------|
| finalizeLedgerTxnChanges | 164ms | 136ms | **-28ms (-17%)** |
| addLiveBatch | 119ms | 93ms | **-26ms (-22%)** |
| mergeInMemory | 86ms | 61ms | **-25ms (-29%)** |
| mergeInMemory put loop | N/A | 42ms | New zone |
| mergeInMemory merge | N/A | 11ms | New zone |
| mergeInMemory index future wait | N/A | 2.2µs | New zone — confirms full overlap |
| InMemoryIndex (from state, line 82) | 22ms | 22ms | Same (now on worker thread) |
| getBucket | 1.3ms | 1.4ms | Same (skips index build) |

#### Analysis

The parallelization works exactly as designed:

1. **Index construction fully overlapped**: The `mergeInMemory index future wait`
   zone averages just 2.2µs (max 2.7µs), meaning the async index construction
   always finishes well before the put loop completes. The full ~22ms of index
   construction is hidden behind the ~42ms put loop.

2. **mergeInMemory dropped 25ms**: From 86ms → 61ms, matching the ~22ms
   InMemoryIndex construction time that is now overlapped.

3. **addLiveBatch dropped 26ms**: From 119ms → 93ms, propagating the
   mergeInMemory improvement upward.

4. **finalizeLedgerTxnChanges dropped 28ms**: From 164ms → 136ms (includes
   the prior experiment 003's parallel InMemorySorobanState update). The
   commit path is now ~84ms faster than the original sequential ~220ms.

5. **No TPS change**: The binary search step is 64 TPS. The 28ms saving on a
   ~1000ms ledger close may not be enough to cross the next threshold, or the
   bottleneck has shifted elsewhere (e.g., `applySorobanStageClustersInParallel`
   at 752ms/call dominates the ledger close).

## Thread Safety
- `mergedEntries`: Both threads read (const ref). No mutation. Safe.
- `meta` (BucketMetadata): Read by index constructor (const ref). Safe.
- `bucketManager`: Passed to `LiveBucketIndex` constructor — only used for
  `getCacheHitMeter()`/`getCacheMissMeter()` which return references to
  existing medida::Meter objects. Safe.
- Put loop's `BucketOutputIterator`: Writes to its own file/hasher. No shared
  state with index construction. Safe.

## Files Changed
- `src/bucket/LiveBucket.cpp` — parallel index construction in mergeInMemory,
  Tracy zones, `#include <future>`
- `src/bucket/BucketOutputIterator.cpp` — preBuiltIndex parameter in getBucket
- `src/bucket/BucketOutputIterator.h` — updated getBucket declaration

## Verdict
**Success.** While TPS did not cross the next binary search threshold, Tracy
confirms a real 25-28ms per-ledger reduction in the commit path. Combined with
experiment 003 (parallel InMemorySorobanState), the commit path has been reduced
from ~220ms to ~136ms — a cumulative 38% reduction.
