# Experiment 060: Cache TTL key hash in InternalContractDataMapEntry ValueEntry

## Date
2026-02-24

## Hypothesis
`InMemorySorobanState::updateState` takes ~78ms per ledger (on a worker thread
concurrent with `addLiveBatch`). The `mContractDataEntries` unordered_set uses
`InternalContractDataMapEntry` which indexes by TTL key hash. The `ValueEntry`
subclass recomputes `getTTLKey()` (XDR serialize + SHA-256 hash) on every call
to `copyKey()` and `hash()` — meaning every hash bucket lookup, equality
comparison, and rehash triggers SHA-256. With ~128K+ CONTRACT_DATA entries per
ledger, this costs ~300ns × N entries × 2+ calls per entry = 38-77ms.

By caching the `uint256` TTL key hash in `ValueEntry` at construction time,
all subsequent hash/equality operations become O(1) lookups of the cached value.

## Change Summary
1. **`InMemorySorobanState.h`**: Added `uint256 mCachedKeyHash` field to
   `ValueEntry`, computed once in the public constructor via `getTTLKey()`.
   Changed `copyKey()` and `hash()` to return the cached value instead of
   recomputing. Added a private constructor accepting a pre-computed hash,
   used by `clone()` to avoid recomputation during copy.

## Results

### TPS
- Baseline: 18,944 TPS (experiment 059)
- Post-change: 18,944 TPS
- Delta: 0 TPS (addLiveBatch is the longer concurrent leg)

### Tracy Analysis
- `updateState` self-time: 255ms → 63.7ms/ledger (baseline 310ms → 77.6ms/ledger) — **-13.9ms/ledger (-18%)**
- `finalizeLedgerTxnChanges` total: ~161ms (unchanged — addLiveBatch 115ms is the longer concurrent leg, updateState 64ms finishes first)
- `applyLedger` avg: ~988ms (baseline: ~977ms) — within noise
- Other zones: within run-to-run noise

## Why TPS Didn't Change
`updateState` runs concurrently with `addLiveBatch` via `std::async`.
`addLiveBatch` takes ~115ms/ledger (including `convertToBucketEntry` 41ms,
`freshInMemoryOnly` 42ms, bucket merge setup). `updateState` at 64ms now
finishes well before the main thread's `addLiveBatch` completes, so the
worker thread's improvement is fully hidden. The `waitForInMemoryUpdate`
shows ~0us because the worker finishes during addLiveBatch.

This improvement will compound when addLiveBatch is eventually optimized —
once addLiveBatch drops below ~64ms, updateState becomes the bottleneck
and this 14ms saving directly reduces wall-clock time.

## Files Changed
- `src/ledger/InMemorySorobanState.h` — Cached `uint256 mCachedKeyHash` in
  `ValueEntry`; changed `copyKey()`/`hash()`/`clone()` to use cached value

## Commit
