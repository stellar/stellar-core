# Experiment 031: Replace std::set with UnorderedSet for Eviction Modified Keys

## Date
2026-02-21

## Hypothesis
`resolveBackgroundEvictionScan` takes `LedgerKeySet` (a `std::set<LedgerKey,
LedgerEntryIdCmp>`) built by `getAllKeysWithoutSealing()`. With ~30K modified
keys per ledger, building the ordered set costs O(n log n) with an expensive
comparison function (`LedgerEntryIdCmp` does `lexCompare` on CONTRACT_DATA
fields), and each lookup in the filtering loop costs O(log n). Switching to
`UnorderedSet<LedgerKey>` gives O(n) construction and O(1) lookups.

## Change Summary
Changed `getAllKeysWithoutSealing()` return type from `LedgerKeySet`
(`std::set<LedgerKey, LedgerEntryIdCmp>`) to `UnorderedSet<LedgerKey>`.
Changed `resolveBackgroundEvictionScan()` parameter to match. Added
`reserve(mEntry.size())` for the unordered set to avoid rehashing.

Neither caller (eviction resolution, TransactionMeta operation keys) depends
on ordering — both only use `find()` lookups. The hash function
`std::hash<LedgerKey>` was already defined in `LedgerHashUtils.h`.

## Results

### TPS
- Baseline (exp-030): 14,976 TPS [14,976-15,104]
- Post-change: 14,976 TPS [14,976-15,104]
- Delta: Within variance (0%)

### Tracy Analysis
| Zone | Exp-030 (ms/ledger) | Exp-031 (ms/ledger) | Delta |
|------|---------------------|---------------------|-------|
| finalize: resolveEviction | 46.6 | 19.6 | -27.0 (-58%) |
| sealLedgerTxnAndStoreInBucketsAndDB | 185.8 | 155.2 | -30.6 (-16.5%) |
| applyLedger | 1261.7 | 1222.1 | -39.6 (-3.1%) |

The per-ledger improvement is clear in Tracy (-27ms in resolveEviction alone).
TPS didn't visibly move because the binary search resolution is limited and
benchmark variance (~5-10%) masks per-ledger improvements under ~50ms.

## Files Changed
- `src/ledger/LedgerTxn.h` — Changed `getAllKeysWithoutSealing` virtual return
  type from `LedgerKeySet` to `UnorderedSet<LedgerKey>`
- `src/ledger/LedgerTxnImpl.h` — Changed impl return type
- `src/ledger/LedgerTxn.cpp` — Changed implementation to build
  `UnorderedSet<LedgerKey>` with `reserve()`
- `src/bucket/BucketManager.h` — Changed `resolveBackgroundEvictionScan`
  parameter from `LedgerKeySet const&` to `UnorderedSet<LedgerKey> const&`;
  added `UnorderedSet.h` and `LedgerHashUtils.h` includes
- `src/bucket/BucketManager.cpp` — Changed parameter type
- `src/bucket/test/BucketTestUtils.cpp` — Updated test to use
  `UnorderedSet<LedgerKey>` instead of `LedgerKeySet`

## Commit
