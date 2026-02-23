# Experiment 048: Move Semantics in commitChangesToLedgerTxn

## Date
2026-02-23

## Hypothesis
`commitChangesToLedgerTxn` (44ms/ledger) copies every LedgerEntry twice when
committing from the parallel apply global state into the LedgerTxn: once to
create an `InternalLedgerEntry` from the scoped optional, and once inside
`make_shared<InternalLedgerEntry>` within `createWithoutLoading`/
`updateWithoutLoading`. Since `commitChangesToLedgerTxn` is called after all
stages complete and the global state is immediately destroyed, we can safely
move entries instead of copying.

## Change Summary
1. Added `InternalLedgerEntry(LedgerEntry&&)` move constructor to avoid
   deep-copying XDR data when constructing from a temporary.
2. Added `ScopedLedgerEntryOpt::moveFromScope()` method that moves the
   underlying `optional<LedgerEntry>` out of the scope wrapper (with scope
   ID validation), instead of the read-only `readInScope()`.
3. Added `createWithoutLoading(InternalLedgerEntry&&)` and
   `updateWithoutLoading(InternalLedgerEntry&&)` move overloads to
   `AbstractLedgerTxn` (with default forwarding) and `LedgerTxn` (with
   optimized `make_shared(std::move(...))` implementation).
4. Made `commitChangesToLedgerTxn` non-const and changed it to use
   `moveFromScope` → move-construct `InternalLedgerEntry` → move into
   LedgerTxn, eliminating both deep copies per entry.

## Results

### TPS
- Baseline: 16,960 TPS
- Post-change: 17,216 TPS
- Delta: +1.5% / +256 TPS

### Tracy Analysis
- `commitChangesToLedgerTxn`: 44.3ms → 38.6ms per ledger (-12.8%)
- `applyLedger`: 1,071ms → 1,051ms per ledger (-1.9%)
- `applySorobanStageClustersInParallel` self-time: 526ms → 506ms (-3.8%)

## Files Changed
- `src/ledger/InternalLedgerEntry.h` — added `InternalLedgerEntry(LedgerEntry&&)` constructor
- `src/ledger/InternalLedgerEntry.cpp` — implemented move constructor
- `src/ledger/LedgerEntryScope.h` — added `moveFromScope` to `ScopedLedgerEntryOpt`, added `scopeMoveOptionalEntry` to `LedgerEntryScope`
- `src/ledger/LedgerEntryScope.cpp` — implemented `moveFromScope` and `scopeMoveOptionalEntry`
- `src/ledger/LedgerTxn.h` — added move overloads for `createWithoutLoading`/`updateWithoutLoading` in `AbstractLedgerTxn` and `LedgerTxn`
- `src/ledger/LedgerTxnImpl.h` — added move overloads for `LedgerTxn::Impl`
- `src/ledger/LedgerTxn.cpp` — implemented default base class forwarding and optimized `LedgerTxn` move implementations
- `src/transactions/ParallelApplyUtils.h` — changed `commitChangesToLedgerTxn` from const to non-const
- `src/transactions/ParallelApplyUtils.cpp` — use `moveFromScope` + move semantics throughout

## Commit
<pending>
