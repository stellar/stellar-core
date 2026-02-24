# Experiment 056: In-place Mutation in InMemorySorobanState

## Date
2026-02-24

## Hypothesis
`updateContractDataTTL` and `updateContractData` in InMemorySorobanState use
erase+emplace to modify entries in the unordered_set. Each erase+emplace cycle
involves:
1. SHA-256 recomputation for the new entry's hash (~200ns per call)
2. Memory allocation for a new ValueEntry + unique_ptr
3. Memory deallocation for the old ValueEntry

With ~36K TTL updates + ~18K data updates per ledger = ~54K erase+emplace
cycles, this wastes ~200ns * 54K = 10.8ms in SHA-256 alone, plus allocation
overhead.

Since TTLData and LedgerEntry are not part of the hash key (hash is based on
the TTL key hash which is derived from the ContractData key), we can modify
these fields in-place without invalidating the unordered_set invariants.

`unique_ptr<T>::operator*() const` returns `T&` (non-const), providing shallow
const semantics that allow mutation of pointed-to data through the set's const
iterators.

## Change Summary
- Removed `const` from `ContractDataMapEntryT::ledgerEntry` and
  `ContractDataMapEntryT::ttlData` to allow in-place mutation
- Added `updateTTLData()` and `updateLedgerEntryPtr()` virtual methods to
  `AbstractEntry`, `ValueEntry` (implements), and `QueryKey` (throws)
- Added corresponding public methods to `InternalContractDataMapEntry`
- Changed `updateContractDataTTL` from erase+emplace to in-place TTL update
- Changed `updateContractData` from erase+emplace to in-place LedgerEntry
  pointer swap

## Results

### TPS
- Baseline: 17,984 TPS
- Post-change: 18,368 TPS (confirmed across 2 runs)
- Delta: +384 TPS (+2.1%)

### Tracy Analysis
- `updateState` self-time: 309ms / 4 = 77.2ms/call (baseline: 355ms / 4 = 88.7ms)
  — **-11.5ms (-13%)**
- `addLiveBatch` avg: 111.5ms (baseline: 120ms) — **-8.5ms (-7.1%)**
  Likely due to reduced memory allocator contention with concurrent updateState
- `applyLedger` avg: 1,005ms (baseline: 1,013ms) — -8ms (-0.8%)
- `finalizeLedgerTxnChanges` avg: 157.8ms (baseline: ~160ms) — -2.2ms

## Why It Worked
The erase+emplace pattern was especially expensive because:
1. Each emplace triggered SHA-256 recomputation in `ValueEntry::hash()` via
   `copyKey()` → `getTTLKey()` (~200ns each)
2. Each cycle allocated+deallocated a ValueEntry (unique_ptr + shared_ptr
   reference counting)
3. The concurrent `addLiveBatch` thread contended with updateState for the
   memory allocator

By mutating in-place, we eliminate all three costs. The 11.5ms reduction in
updateState is consistent with ~54K eliminated SHA-256 computations (~10.8ms)
plus allocation savings.

The addLiveBatch improvement (8.5ms) is a secondary benefit from reduced
allocator contention — both threads previously competed for malloc/free locks.

## Files Changed
- `src/ledger/InMemorySorobanState.h` — Removed const from ContractDataMapEntryT
  fields, added in-place mutation methods to AbstractEntry/ValueEntry/QueryKey/
  InternalContractDataMapEntry
- `src/ledger/InMemorySorobanState.cpp` — Changed updateContractDataTTL and
  updateContractData to use in-place mutation

## Commit
