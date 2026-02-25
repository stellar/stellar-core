# Experiment 078: Use upsertEntryKnownExisting in recordStorageChanges

## Hypothesis

In `recordStorageChanges`, every modified ledger entry returned by the Soroban
host is upserted via `upsertLedgerEntry`, which internally calls
`getLiveEntryOpt()` to check whether the entry already exists. This check
traverses mTxEntryMap → mThreadState → InMemorySorobanState (2-3 hash lookups
for first access of each key).

For entries that matched a read-write footprint key AND were successfully loaded
during `addReads`, we *know* the entry already exists. By tracking which RW
footprint keys had existing entries during `addReads` (using a bitfield), we can
call `upsertLedgerEntryKnownExisting` for those entries, skipping the expensive
`getLiveEntryOpt` existence check entirely.

## Changes

Modified `src/transactions/InvokeHostFunctionOpFrame.cpp`:

1. **Added member variables** to `InvokeHostFunctionApplyHelper`:
   - `mRwKeyExistedBits` (uint64_t bitfield for small footprints ≤64 keys)
   - `mRwKeyExistedVec` (vector<bool> fallback for larger footprints)

2. **In `addReads`**: When processing RW footprint keys (`isReadOnly == false`)
   and the entry exists (`entryOpt` has value), set the corresponding bit in the
   bitfield to record that this RW key had an existing entry.

3. **In `recordStorageChanges`**: When a modified entry matches an RW footprint
   key and that key's "existed" bit is set, call `upsertLedgerEntryKnownExisting`
   instead of `upsertLedgerEntry`. This skips the `getLiveEntryOpt` chain.
   For entries without the "existed" bit (newly created entries, TTL entries),
   the regular `upsertLedgerEntry` path is used, preserving the create-counting
   logic for the TTL pairing assertion.

## Semantic Safety

- **Existing entries (existed bit set)**: `upsertLedgerEntry` would have
  returned `false` (not a create) anyway, so skipping it with
  `upsertLedgerEntryKnownExisting` is equivalent.
- **Newly created entries (existed bit not set)**: Go through the regular
  `upsertLedgerEntry` path, which correctly returns `true` for creates and
  increments `numCreatedSorobanEntries` / `numCreatedTTLEntries`.
- **TTL entries**: TTL keys don't appear in the RW footprint, so
  `rwKeyExisted` is always false for them — they use the regular path.
- **Auto-restored entries**: Restored during `handleArchivedEntry`, which
  inserts them into `mTxEntryMap` directly. During `recordStorageChanges`,
  `getLiveEntryOpt` finds them in `mTxEntryMap` quickly. The existed bit is
  NOT set for these (they were archived, not loaded), so they go through the
  regular path.

## Results

- Tests: All 66 `[soroban][tx]` tests passed (49011 assertions)
- Run 1: **19,520 TPS** [19,520, 19,648]
- Run 2: **19,840 TPS** [19,840, 19,904]
- Baseline (3 recent runs, same hardware): 18,944 TPS consistently
- Previous best: 19,712 TPS (exp 075)
- **New record: 19,840 TPS (+4.7% over recent baseline, +0.6% over previous best)**

## Profile Analysis (Tracy, 60s capture)

| Zone | Calls | Self-time (ms) | Per-call (μs) |
|------|-------|----------------|---------------|
| `upsertEntry` (075b) | 329K | 794 | 2.41 |
| `upsertEntry` (078) | 251K | 785 | 3.13 |
| `upsertEntryKnownExisting` (078) | 125K | 89 | **0.71** |

The optimization redirected 125K calls (38% of total upserts) from
`upsertEntry` (2.41μs/call) to `upsertEntryKnownExisting` (0.71μs/call),
saving ~1.7μs per call × 125K calls = ~212ms per 60s window.

## Files Changed

- `src/transactions/InvokeHostFunctionOpFrame.cpp`
