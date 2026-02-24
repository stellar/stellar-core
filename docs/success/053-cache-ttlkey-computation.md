# Experiment 053: Cache getTTLKey Computation in Thread State

## Date
2026-02-24

## Hypothesis
`getTTLKey()` performs SHA-256 + XDR serialization for every Soroban entry. In
the parallel apply path, the same TTL keys are recomputed redundantly:
1. In `collectClusterFootprintEntriesFromGlobal` (once per key per TX in cluster)
2. In `buildRoTTLSet` (once per RO key per TX during commit)
3. In `flushRoTTLBumpsInTxWriteFootprint` (once per RW key per TX)

For SAC transfers where all TXs share the same RO footprint (contract instance,
code), this means ~17K redundant SHA-256 computations per stage for just 2 unique
RO keys. Total redundant getTTLKey calls across all paths: ~170K+ per stage.

By caching the mapping from data/code keys to TTL keys in a per-cluster
`UnorderedMap`, we eliminate redundant SHA-256 and also remove the per-TX
`UnorderedSet<LedgerKey>` allocation in `buildRoTTLSet`, replacing it with a
direct linear scan of the TX's RO footprint (2-4 entries for SAC transfers).

## Change Summary
- Added `mTTLKeyCache` (`UnorderedMap<LedgerKey, LedgerKey>`) member to
  `ThreadParallelApplyLedgerState`
- Modified `collectClusterFootprintEntriesFromGlobal` to populate the cache
  via `try_emplace`, computing `getTTLKey` only for newly-seen keys
- Replaced per-TX `buildRoTTLSet` (which allocated `UnorderedSet` and called
  `getTTLKey` per RO entry) with a direct linear scan of the TX's RO footprint
  using cached TTL key lookups
- Updated `flushRoTTLBumpsInTxWriteFootprint` to use cache instead of
  `getTTLKey`
- Removed the now-unused `buildRoTTLSet` free function

## Results

### TPS
- Baseline: 17,984 TPS
- Post-change run 1 (cache + per-TX set): 18,368 TPS
- Post-change run 2 (cache + linear scan): 17,792 TPS
- Delta: Within variance (~0% net)

### Tracy Analysis
- Per-TX `parallelApply` mean: ~115µs (unchanged)
- `applyLedger` mean: ~1,015ms (unchanged)
- The SHA-256 savings (~100-200ns/TX) are below the binary search resolution
  (64 TPS steps ≈ 3.5ms per ledger at 18K TXs)

### Why Marginal
The per-TX cost of `buildRoTTLSet` with 2 RO Soroban entries was only ~400-600ns
(2x SHA-256 at ~200ns each + UnorderedSet construction). At 18K TXs per ledger
this totals ~7-11ms, but only ~5ms is saved (the cache lookup has its own cost).
5ms savings on a 1000ms target is 0.5%.

## Files Changed
- `src/transactions/ParallelApplyUtils.h` - Added mTTLKeyCache member, updated
  commitChangeFromSuccessfulTx signature
- `src/transactions/ParallelApplyUtils.cpp` - Populated cache in
  collectClusterFootprintEntriesFromGlobal, replaced buildRoTTLSet with linear
  scan, updated flushRoTTLBumpsInTxWriteFootprint

## Commit
(committed with this file)
