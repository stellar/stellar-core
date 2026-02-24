# Experiment 064: Parallel Sort in convertToBucketEntry

## Date
2026-02-24

## Hypothesis
`convertToBucketEntry` spends 44ms/ledger sorting ~128K entries using sequential
`std::sort`. By splitting into 4 chunks, sorting each on a separate thread, and
merging with `std::inplace_merge`, the sort wall-clock time should drop from
O(n log n) to O(n/4 * log(n/4)) + O(n) merge, saving ~25ms.

## Change Summary
Modified `LiveBucket::convertToBucketEntry` to use parallel sort:
- Split refs vector into 4 chunks
- Sort 3 chunks on worker threads via `std::async`, 1 on main thread
- Merge pairs in parallel, then final merge via `std::inplace_merge`
- Fallback to sequential sort for small arrays (<4096 entries)

## Results

### TPS
- Baseline: 19,328 TPS (avg of 19,520 + 19,136)
- Run 1: 19,264 TPS
- Run 2: 19,520 TPS
- Average: 19,392 TPS
- Delta: **+0.3% (noise)**

### Tracy Analysis
- `convertToBucketEntry`: 44ms → 29ms/ledger (**-34%**) — sort itself improved
- `addBatchInternal`: 113ms → 108ms/ledger (-4.4%) — variance-dominated
- `mergeInMemory`: 66ms → 75ms (+14%) — run-to-run variance
- `applyLedger`: 963ms → 970ms (~same)

## Why It Failed
Same root cause as experiment 044: addLiveBatch is dominated by the merge + put
loop (41ms + 21ms), not the sort (29ms). Even with a 34% faster sort,
addLiveBatch (108ms) remains well above updateState (69ms), so the concurrent
max barely changes. The 5ms addLiveBatch improvement is <1% of applyLedger.

The merge overhead of `std::inplace_merge` (3 merge passes) partially offsets
the parallel sort gains. Run-to-run variance in the merge + put loop masks the
improvement.

### Key Insight
To make sort improvements translate to TPS, addLiveBatch must drop BELOW
updateState (~69ms). This requires eliminating ALL of the sort (44ms) AND
~10ms from the merge/put loop — total savings needed: >44ms from addLiveBatch.
The parallel sort only saves 15ms, leaving addLiveBatch at ~98ms (theory) or
108ms (measured with variance).

## Files Changed (REVERTED)
- `src/bucket/LiveBucket.cpp` — parallel sort in convertToBucketEntry
