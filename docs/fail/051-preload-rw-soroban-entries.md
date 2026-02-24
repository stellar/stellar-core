# Experiment 051: Pre-load ReadWrite Soroban Entries into Global Map

## Date
2026-02-23

## Hypothesis
Experiment 050 pre-loaded ReadOnly Soroban entries into the global parallel
apply state, saving redundant InMemorySorobanState lookups. Extending this
to also pre-load ReadWrite entries (balance entries, their TTLs) should further
reduce per-TX lookup overhead in `addReads` by eliminating ~64K+
InMemorySorobanState::get() calls across 4 threads.

## Change Summary
Extended the pre-loading loop in `GlobalParallelApplyLedgerState` constructor
to iterate both `footprint.readOnly` and `footprint.readWrite` keys. For each
Soroban entry, pre-loaded the entry and its TTL. For TTL entries appearing
directly in the footprint, pre-loaded them as well.

## Results

### TPS
- Baseline: 18,368 TPS (exp050)
- Post-change: 16,960 TPS [16,960, 17,024]
- Delta: **-7.7% / -1,408 TPS (REGRESSION)**

## Why It Failed
The optimization only works for **shared** entries — entries referenced by many
TXs (like contract code/instance in RO footprints). For RW entries, each TX
has **unique** balance entries. This means:

1. **Same number of InMemorySorobanState loads**: Pre-loading 32K+ unique RW
   entries does the same number of InMemorySorobanState::get() calls as per-TX
   loading — it just moves them from TX execution time to setup time.

2. **Added overhead**: Pre-loaded entries are copied into the global map during
   setup, then copied from global → thread map in
   `collectClusterFootprintEntriesFromGlobal`, then copied from thread → TX
   scope during per-TX execution. This ADDS copies compared to the direct
   InMemorySorobanState path.

3. **Memory pressure**: Loading 32K+ entries into the global map increases
   memory usage and hash table size, causing slower lookups for ALL entries
   (including the beneficial RO entries).

### Key Lesson
Pre-loading into the global map is only beneficial for entries with high
sharing ratio (many TXs referencing the same entry). For unique-per-TX entries,
it adds overhead without reducing total lookups.

## Files Changed
- `src/transactions/ParallelApplyUtils.cpp` — extended pre-loading to RW entries (REVERTED)
