# Experiment 063: Avoid Building modifiedKeys Set for Eviction

## Date
2026-02-24

## Hypothesis
`resolveBackgroundEvictionScan` receives an `UnorderedSet<LedgerKey>` built by
`getAllKeysWithoutSealing()` containing ~128K entries (~20ms to build). However,
the eviction scan only performs ~10-100 lookups into this set (checking whether
eviction candidates have been modified). Building a 128K-entry hash set for
a handful of lookups is wasteful. Direct O(1) lookups into the LedgerTxn's
existing EntryMap would eliminate the set construction entirely.

## Change Summary
Added `isModifiedKey(LedgerKey const&)` method to `AbstractLedgerTxn` /
`LedgerTxn` that performs an O(1) lookup directly in the LedgerTxn's internal
`mEntry` map. Created two overloads of `resolveBackgroundEvictionScan`:

1. **Production path** (no set parameter): Uses `ltx.isModifiedKey()` for
   direct EntryMap lookups. Called from `LedgerManagerImpl::finalizeLedgerTxnChanges`.
2. **Test path** (with `UnorderedSet<LedgerKey>` parameter): For test helpers
   like `BucketTestUtils` that don't write entries through the LedgerTxn
   subsystem and need to provide their own key set.

The production path completely eliminates the `getAllKeysWithoutSealing()` call
and its ~20ms per-ledger cost.

## Results

### TPS
- Baseline: 18,944 TPS
- Run 1: 19,520 TPS
- Run 2: 19,136 TPS
- Average: 19,328 TPS
- Delta: +384 TPS (+2.0%)

### Tracy Analysis
- `finalize: resolveEviction`: 20ms → 0.116ms/ledger (**99.4% reduction**)
- `getAllKeysWithoutSealing` zone completely eliminated (was ~20ms)
- `resolveBackgroundEvictionScan`: 0.116ms (down from ~20ms)
- Total `applyLedger` improvement dampened because eviction ran partially
  concurrently with other work

## Files Changed
- `src/ledger/LedgerTxn.h` — Added `isModifiedKey` pure virtual to
  `AbstractLedgerTxn`, override in `LedgerTxn`
- `src/ledger/LedgerTxnImpl.h` — Added `isModifiedKey` declaration to
  `LedgerTxn::Impl`
- `src/ledger/LedgerTxn.cpp` — Added `isModifiedKey` implementation (O(1)
  EntryMap lookup via `mEntry.find(InternalLedgerKey(key))`)
- `src/bucket/BucketManager.h` — Added two overloads of
  `resolveBackgroundEvictionScan` (production + test)
- `src/bucket/BucketManager.cpp` — Implemented both overloads; production
  path uses lambda capturing `ltx.isModifiedKey()`
- `src/ledger/LedgerManagerImpl.cpp` — Removed `getAllKeysWithoutSealing()`
  call, uses production overload
- `src/invariant/test/InvariantTests.cpp` — Updated to use production overload
- `src/bucket/test/BucketTestUtils.cpp` — Uses test overload with explicit
  key set

## Commit
<to be filled after commit>
