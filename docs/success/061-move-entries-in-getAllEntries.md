# Experiment 061: Move entries instead of copying in getAllEntries

## Date
2026-02-24

## Hypothesis
`getAllEntries` deep-copies ~128K+ `LedgerEntry` objects from the `EntryMap`
into three output vectors (init, live, dead) at ~19ms per ledger. Since the
`LedgerTxn` is immediately sealed after `getAllEntries` (the entries are never
accessed again), we can `std::move` the `LedgerEntry` objects instead of
copying them. For XDR-generated types containing `xdr::xvector`, move is O(1)
pointer transfer vs O(N) deep copy.

The key insight: `LedgerEntryPtr::operator->() const` returns a non-const
`InternalLedgerEntry*`, and `InternalLedgerEntry::ledgerEntry()` has a
non-const overload returning `LedgerEntry&`. So `std::move(entry->ledgerEntry())`
works even through the `EntryMap const&` reference in the existing
`maybeUpdateLastModifiedThenInvokeThenSeal` lambda — no signature changes needed.

## Change Summary
1. **`LedgerTxn.cpp`**: Changed `getAllEntries` to use
   `std::move(entry->ledgerEntry())` in the two `emplace_back` calls for
   init and live entries. Added comment explaining the safety rationale
   (LedgerTxn is sealed after, entries never accessed again).

## Results

### TPS
- Baseline: 18,944 TPS (experiment 060)
- Post-change run 1: 18,688 TPS
- Post-change run 2: 18,368 TPS
- Delta: within noise (exp 059 also showed 18,368/18,944 variance)

### Tracy Analysis
- `getAllEntries` self-time: 43.7ms → 10.9ms/ledger (baseline 76ms → 19ms/ledger) — **-8.1ms/ledger (-43%)**
- `applyLedger` avg: ~970ms (baseline: ~988ms) — **-18ms/ledger (-1.8%)**
- `addLiveBatch`: 115.3ms/ledger (unchanged — downstream consumers unaffected)
- `updateInMemorySorobanState`: 67.0ms/ledger (baseline: 64ms — within noise)
- `finalize: waitForInMemoryUpdate`: ~0ms (unchanged)
- `finalize: resolveEviction`: 19.8ms/ledger (unchanged)

## Why TPS Didn't Change
The 8ms saving on the serial path is < 1% of the ~988ms `applyLedger` total.
The binary search resolution at ~18,944 TPS has 128 TPS steps, each adding
~7ms. An 8ms saving is just barely one step, well within the benchmark's
5-10% run-to-run variance. The improvement compounds with other serial path
optimizations.

## Files Changed
- `src/ledger/LedgerTxn.cpp` — Changed `getAllEntries` to move entries instead
  of copying (two `emplace_back` calls changed to use `std::move`)

## Commit
