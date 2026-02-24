# Experiment 036b: Move Overloads for commitChangesToLedgerTxn

## Date
2025-02-22

## Hypothesis
In `commitChangesToLedgerTxn`, each entry is copied twice:
1. `LedgerEntry` -> `InternalLedgerEntry ile(...)` (stack copy)
2. `InternalLedgerEntry` -> `make_shared<InternalLedgerEntry>(entry)` (heap copy via createWithoutLoading/updateWithoutLoading)

Adding move overloads for `createWithoutLoading` and `updateWithoutLoading` would eliminate COPY 2 by moving the stack-constructed `InternalLedgerEntry` directly into the `shared_ptr`.

## Change Summary
Added `InternalLedgerEntry&&` move overloads to:
- `AbstractLedgerTxn` (pure virtual in LedgerTxn.h)
- `LedgerTxn` and `LedgerTxn::Impl` (LedgerTxn.h, LedgerTxn.cpp, LedgerTxnImpl.h)
- `InMemoryLedgerTxn` (test class)

Modified `commitChangesToLedgerTxn` to use `std::move(ile)` when calling these overloads.

## Results

### TPS
- Baseline: 15,808 TPS
- Post-change: 15,808 TPS
- Delta: 0%

### Tracy Analysis
- `commitChangesToLedgerTxn` self-time: 65,245,005 -> 64,465,178 ns (-780us, -1.2%)
- Per-ledger saving: ~0.8ms out of 76ms total — negligible

## Why It Failed
SAC balance entries are small `CONTRACT_DATA` LedgerEntries. The `InternalLedgerEntry` copy cost is minimal (~16ns per entry based on calculations). The `make_shared` allocation overhead dominates the cost, not the data copy. Moving the data saves a trivial amount since the payload is small.

The real cost in `commitChangesToLedgerTxn` comes from:
1. Map lookups (`mInMemorySorobanState.get()`, `getNewestVersionBelowRoot()`)
2. The `insert_or_assign` into the LedgerTxn entry map
3. The `make_shared` allocation itself (not the copy into it)

## Files Changed (REVERTED)
- `src/ledger/LedgerTxn.h` — added move overload declarations
- `src/ledger/LedgerTxn.cpp` — added move overload implementations
- `src/ledger/LedgerTxnImpl.h` — added Impl move overload declarations
- `src/ledger/test/InMemoryLedgerTxn.h` — added test class move overloads
- `src/ledger/test/InMemoryLedgerTxn.cpp` — added test class move overload implementations
- `src/transactions/ParallelApplyUtils.cpp` — used std::move in commitChangesToLedgerTxn
