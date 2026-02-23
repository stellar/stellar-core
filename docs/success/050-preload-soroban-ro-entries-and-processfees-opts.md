# Experiment 050: Pre-load Soroban RO Entries + processFeesSeqNums Optimizations

## Date
2026-02-23

## Hypothesis
Three small optimizations combined:

1. **Pre-load Soroban read-only entries into global parallel apply state**: During
   parallel apply, every TX in every thread that reads a Soroban RO entry (contract
   instance, code, TTL) must look it up through
   `InMemorySorobanState::get()` — involving hash computation + LedgerEntry copy.
   These entries are constant across all TXs. Pre-loading them into
   `mGlobalEntryMap` during setup means `collectClusterFootprintEntriesFromGlobal`
   copies them into thread maps, and subsequent per-TX lookups hit thread-local
   maps instead of traversing to InMemorySorobanState. Expected: reduce
   `upsertEntry` self-time.

2. **Cache protocol version in processFeesSeqNums**: The inner loop calls
   `loadHeader()` per TX to check protocol version. Caching the version before
   the loop avoids repeated header loads.

3. **Skip Soroban merge tracking in processFeesSeqNums**: Soroban TXs cannot
   have merge operations (they use a single source account with a single seqnum).
   Skipping the `accToMaxSeq` map tracking for Soroban TXs avoids unnecessary
   map lookups in the hot loop.

4. **Move mLatestTxResultSet instead of copying**: The result set is no longer
   needed after assignment; std::move avoids a deep copy.

## Change Summary
1. In `ParallelApplyUtils.cpp`, added "fetchSorobanReadOnlyEntries from footprints"
   section after existing classic entries fetch. Iterates all RO Soroban keys
   from TX footprints, loads from InMemorySorobanState or snapshot, and stores
   in `mGlobalEntryMap`. Also pre-loads corresponding TTL entries.

2. In `LedgerManagerImpl.cpp:processFeesSeqNums`, cached `cachedLedgerVersion`
   and `isV19OrLater` before the loop. Skips accToMaxSeq tracking for Soroban TXs.

3. In `LedgerManagerImpl.cpp`, changed `mLatestTxResultSet = txResultSet` to
   `std::move(txResultSet)`.

## Results

### TPS
- Baseline: 17,216 TPS
- Post-change: 18,368 TPS [18,368, 18,496]
- Delta: +6.7% / +1,152 TPS

### Tracy Analysis
- `applyLedger`: 1,047ms -> 1,019ms per ledger (-2.7%)
- `processFeesSeqNums`: 60.4ms -> 51.9ms per ledger (-14.1%)
- `upsertEntry` self-time: 446ms -> 417ms (-6.5%)
- `applySorobanStageClustersInParallel`: 600ms -> 574ms (-4.3%)
- `fetchSorobanReadOnlyEntries from footprints`: 2.9ms (new, setup cost)
- `GlobalParallelApplyLedgerState`: 40ms -> 43.3ms (+8%, includes pre-load)

## Files Changed
- `src/transactions/ParallelApplyUtils.cpp` — pre-load Soroban RO entries into global map
- `src/ledger/LedgerManagerImpl.cpp` — cache protocol version, skip Soroban merge tracking, move result set

## Commit
75b2ca0b0
