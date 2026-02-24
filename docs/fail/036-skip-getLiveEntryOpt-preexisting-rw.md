# Experiment 036: Skip getLiveEntryOpt in upsertEntry for pre-existing RW entries

## Date
2026-02-22

## Hypothesis
In `recordStorageChanges`, each call to `upsertLedgerEntry` invokes
`getLiveEntryOpt` (~500-900ns overhead) to determine whether the entry is
newly created or updated. During `addReads`, we already load all RW footprint
entries and know which exist. By tracking this and using
`upsertLedgerEntryKnownExisting` (which skips `getLiveEntryOpt`) for
pre-existing entries, we can save ~500ns per entry.

## Change Summary
Added `mRWFootprintSize` and `mRWExistingEntries` counters to
`InvokeHostFunctionApplyHelper`. During `addReads(rwKeys)`, tracked how many
entries existed. In `recordStorageChanges`, used `upsertLedgerEntryKnownExisting`
when all RW entries pre-existed.

## Results

### TPS
- Baseline (exp-035): 15,808 TPS [15,808-15,872]
- Post-change: 15,808 TPS [15,808-15,872]
- Delta: 0%

### Tracy Analysis
No change in `upsertEntry` calls — optimization never triggered.

## Why It Failed
Diagnostic logging revealed: `rwSize=2 rwExist=1 preExist=0` for ALL 144K
steady-state SAC transfers. The RW footprint has 2 CONTRACT_DATA entries
(source and destination balance), but only the SOURCE balance exists during
`addReads`. The destination balance is being CREATED by the host (first
transfer to that account), so it doesn't exist yet.

With only 1 out of 2 RW entries pre-existing, the all-or-nothing flag
(`allRWPreExist`) never triggers. Even a per-entry set approach would only
save ~500ns on 1 out of 3 modified entries per TX (the 3rd entry is a TTL
created for the new dest balance), yielding ~8ms/ledger across 4 threads —
well within benchmark noise.

The `upsertEntry` function at 1906ns/call has significant cost from scope
wrapping (`scopeAdoptEntryOpt`) and map operations (`insert_or_assign`),
not just the `getLiveEntryOpt` check. Optimizing only the existence check
doesn't address the bulk of the cost.

## Files Changed (REVERTED)
- `src/transactions/InvokeHostFunctionOpFrame.cpp`
