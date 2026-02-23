# Experiment 045: Track Entry Existence to Skip SHA256 Lookups

## Date
2026-02-23

## Hypothesis
In `commitChangesToLedgerTxn`, each entry needs to be committed as either INIT
(new entry, via `createWithoutLoading`) or LIVE (existing entry, via
`updateWithoutLoading`). The existing code determined this by calling
`mInMemorySorobanState.get(key)` for every dirty entry, which for CONTRACT_DATA
entries creates an `InternalContractDataMapEntry` that calls `getTTLKey()` →
`sha256(xdr_to_opaque(key))`. With ~40K Soroban entries per ledger, this added
~16ms of SHA256 computation per ledger in the sequential commit path.

By tracking whether each entry is "new" (didn't exist in persistent state before
the parallel apply phase) via a `mIsNew` bool flag in `ParallelApplyEntry`, we
can skip the expensive SHA256-based InMemorySorobanState lookups entirely and
use a simple boolean check instead.

## Change Summary
1. Added `bool mIsNew{false}` field to `ParallelApplyEntry<S>` template struct
2. Set `mIsNew = true` when `commitChangeFromSuccessfulTx` processes an entry
   that didn't exist in the previous state (`!oldEntryOpt.has_value()`)
3. Propagated `mIsNew` correctly through all scope transitions:
   - TX → Thread (via `try_emplace` preserving first-touch mIsNew)
   - Thread → Global (preserving mIsNew from first stage)
   - Global → Thread (copying mIsNew in `collectClusterFootprintEntriesFromGlobal`)
4. Used `entry.mIsNew` in `commitChangesToLedgerTxn` instead of the expensive
   `mInMemorySorobanState.get(key)` existence check

Key edge case: In auto-restore → delete → create scenarios, the eraseEntry
call must also receive the correct `isNew` flag, because a subsequent TX that
recreates the entry will preserve the mIsNew from the erase (first touch).

## Results

### TPS
- Baseline: 16,640 TPS [16,640, 16,768]
- Post-change: 16,960 TPS [16,960, 17,024]
- Delta: **+1.9%** (+320 TPS)

### Tracy Analysis
- `commitChangesToLedgerTxn`: 44.2ms/ledger (was 72.6ms) — **-39%**
- `finalizeLedgerTxnChanges`: 154.5ms (was 166.2ms) — **-7%**
- `applyLedger` total: 1,071ms (was 1,078ms) — **-0.7%**

The 28ms savings from commitChangesToLedgerTxn are partially absorbed because
`finalizeLedgerTxnChanges` runs `addLiveBatch` and `updateInMemorySorobanState`
concurrently, and `updateInMemorySorobanState` (81.9ms → 85.7ms) is now
sometimes the bottleneck in that concurrent pair.

## Files Changed
- `src/transactions/TransactionFrameBase.h` — added `mIsNew` field to `ParallelApplyEntry<S>`
- `src/transactions/ParallelApplyUtils.h` — added `bool isNew` param to `upsertEntry` and `eraseEntry`
- `src/transactions/ParallelApplyUtils.cpp` — implemented mIsNew tracking through all scope transitions and used it in `commitChangesToLedgerTxn`
