# Experiment 042: Skip toKey and mActive.find in commitChangesToLedgerTxn

## Date
2026-02-23

## Hypothesis
In `commitChangesToLedgerTxn`, each entry calls `updateWithoutLoading(ile)` which
internally re-extracts the key from the entry via `toKey()` and does an
`mActive.find(key)` lookup in what should be an empty map. Both involve hash
computations that are redundant since we already have the key from the
`mGlobalEntryMap` iteration. Adding `*WithoutLoadingFromKey` methods that accept
the pre-computed `InternalLedgerKey` should save ~7-13ms from 72ms by skipping
per-entry `toKey()` + `mActive.find()` hash computations across ~40K entries.

## Change Summary
Added `createWithoutLoadingFromKey(InternalLedgerKey const&, InternalLedgerEntry const&)`
and `updateWithoutLoadingFromKey(...)` to `AbstractLedgerTxn`, `LedgerTxn`,
`LedgerTxn::Impl`, and `InMemoryLedgerTxn`. These skip `toKey()` and `mActive.find()`,
calling `updateEntry` directly with the provided key.

Modified `commitChangesToLedgerTxn` to construct `InternalLedgerKey(key)` once from
the map key and pass it to the new methods.

## Results

### TPS
- Baseline: 16,640 TPS (experiment 041)
- Post-change: 16,640 TPS [16,640, 16,768]
- Delta: **0%**

### Tracy Analysis
- `commitChangesToLedgerTxn` self-time: 72.1ms/ledger (was 71.9ms) — NO change
- `applyLedger` total: 1,089ms (was 1,072ms) — within variance

## Why It Failed
The `toKey()` and `mActive.find()` costs are negligible compared to the
`make_shared<InternalLedgerEntry>` allocation and `mEntry.emplace()` hash map
insert that dominate `commitChangesToLedgerTxn`. The empty-map `find()` is
nearly free (just hash and check one empty bucket). The `toKey()` extraction
for CONTRACT_DATA involves copying fields from the entry, but these are small
and the copy is fast relative to the allocation overhead.

The real cost breakdown in `commitChangesToLedgerTxn` (72ms for ~40K entries):
1. `make_shared<InternalLedgerEntry>` heap allocation — dominant cost
2. `mEntry.emplace(key, lePtr)` hash map insert — significant
3. `mInMemorySorobanState.get(key)` existence check (SHA256 hash per CONTRACT_DATA) — significant
4. `toKey()` + `mActive.find()` — negligible (this experiment)

## Files Changed (REVERTED)
- `src/ledger/LedgerTxn.h` — added FromKey virtual methods
- `src/ledger/LedgerTxn.cpp` — added FromKey implementations
- `src/ledger/LedgerTxnImpl.h` — added Impl FromKey methods
- `src/ledger/test/InMemoryLedgerTxn.h` — added test overrides
- `src/ledger/test/InMemoryLedgerTxn.cpp` — added test overrides
- `src/transactions/ParallelApplyUtils.cpp` — used FromKey methods
