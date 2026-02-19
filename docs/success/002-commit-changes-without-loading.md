# Experiment 002: Optimize commitChangesToLedgerTxn with WithoutLoading APIs

## Result: SUCCESS — 8,896 → 9,408 TPS (+5.8%)

## Change
Replaced the `load()`-based commit path in `commitChangesToLedgerTxn` with
`createWithoutLoading`/`updateWithoutLoading` APIs, eliminating expensive
root-level `getNewestVersion` lookups (~225ms/8 ledgers in exp001 profile).

### Before
- Created child `LedgerTxn ltxInner(ltx)`
- For each dirty entry: `ltxInner.load(key)` → traverses parent chain to root
  (calls `LedgerTxnRoot::getNewestVersion` = cache/DB lookup)
- Committed child: `ltxInner.commit()`

### After
- Operates directly on `ltx`, no child LedgerTxn needed
- For upsert: checks `ltx.getNewestVersionBelowRoot(key)` (O(1), mEntry only)
  and `InMemorySorobanState::get(key)` for existence, then calls
  `updateWithoutLoading` or `createWithoutLoading` accordingly
- For delete (rare): falls back to `load()` + `erase()` to maintain EXACT
  consistency for BucketList merge semantics
- No `commit()` needed since operating directly on `ltx`

### Key Design Decisions
1. **INIT vs LIVE distinction preserved**: `createWithoutLoading` (INIT) for new
   entries, `updateWithoutLoading` (LIVE) for existing — critical for BucketList
   INITENTRY annihilation logic
2. **Existence check via InMemorySorobanState**: For Soroban entries not in
   `ltx.mEntry`, `mInMemorySorobanState.get(key)` provides O(1) hash map lookup
3. **Delete path unchanged**: `eraseWithoutLoading` sets EXTRA_DELETES which
   breaks `getAllKeysWithoutSealing` in `finalizeLedgerTxnChanges`, so deletes
   still use `load()` + `erase()`

## Files Modified
- `src/transactions/ParallelApplyUtils.cpp` — `commitChangesToLedgerTxn` function

## Benchmark Details
- Platform: same as baseline
- Config: `docs/apply-load-max-sac-tps.cfg` (unchanged)
- Previous (exp001): 8,896 TPS (x=139, ~929ms mean apply)
- Current (exp002): 9,408 TPS (x=147, ~999ms mean apply)
- Tracy profile: `/mnt/xvdf/tracy/exp002-commit-opt.tracy`
- Output log: `/mnt/xvdf/tracy/exp002-commit-opt-output.log`

## Cumulative Progress
| Experiment | TPS | Change | Cumulative |
|-----------|-----|--------|------------|
| Baseline | 7,680 | — | — |
| 001 Sharded verifySig cache | 8,896 | +15.8% | +15.8% |
| 002 commitChanges WithoutLoading | 9,408 | +5.8% | +22.5% |
