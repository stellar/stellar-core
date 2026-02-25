# Experiment 079: Move Semantics in upsertEntry/upsertEntryKnownExisting

## Hypothesis
Eliminating deep copies of `LedgerEntry` in `upsertEntry` and `upsertEntryKnownExisting`
by adding move-semantic overloads throughout the call chain would reduce per-TX overhead.
The copy chain was: `upsertLedgerEntry(key, le)` → `TxParallelApplyLedgerState::upsertEntry(key, entry)` →
`scopeAdoptEntryOpt(entry)` → implicit conversion to `optional<LedgerEntry>` (copy #1) →
`ScopedLedgerEntryOpt` constructor (copy #2). With 125K calls per benchmark window, eliminating
both copies was expected to save measurable time.

## Changes Made
1. Added `scopeAdoptEntryOpt(std::optional<LedgerEntry>&& entry)` overload in `LedgerEntryScope.h/.cpp`
2. Added `upsertEntry(LedgerKey const&, LedgerEntry&&, uint32_t)` and
   `upsertEntryKnownExisting(LedgerKey const&, LedgerEntry&&, uint32_t)` to `TxParallelApplyLedgerState`
3. Added virtual `upsertLedgerEntry(LedgerKey const&, LedgerEntry&&)` and
   `upsertLedgerEntryKnownExisting(LedgerKey const&, LedgerEntry&&)` to `LedgerAccessHelper` base class
   with default fallback to `const&` versions
4. Added overrides in `ParallelLedgerAccessHelper` forwarding to the move-based `TxParallelApplyLedgerState` methods
5. Changed callers in `InvokeHostFunctionOpFrame::recordStorageChanges` (lines 804, 806)
   to use `std::move(le)` after the entry is no longer needed

## Test Results
All 66 tests passed (49011 assertions).

## Benchmark Results
- **Run 1**: 18,944 TPS [18,944, 19,072] — **REGRESSION** vs baseline 19,840
- **Run 2**: 19,520 TPS [19,520, 19,648] — still below baseline 19,840

## Analysis
The move semantics optimization did NOT provide measurable improvement. Possible reasons:
1. XDR-generated `LedgerEntry` types may have trivial/efficient copy constructors making
   the copy nearly as cheap as a move
2. The compiler may already be optimizing away the copies via copy elision (RVO/NRVO)
3. The `ScopedLedgerEntryOpt` wrapper introduces scope-tracking overhead that dominates
   the copy cost
4. The virtual dispatch overhead for the new move overloads may offset any savings
5. Added code complexity (new virtual methods) may reduce compiler optimization opportunities

## Conclusion
**FAILED** — no improvement. Reverted all changes.
