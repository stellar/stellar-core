# Experiment 055: Cache TTL key hash in InternalContractDataMapEntry::ValueEntry

## Date
2026-02-24

## Hypothesis
`ValueEntry::copyKey()` recomputes `getTTLKey()` (SHA-256, ~200ns) on every call.
The unordered_set calls `hash()`/`copyKey()` during find, emplace, and equality
comparison. With ~8 SHA-256 recomputations per TX during `updateState` and 16K
TXs/ledger, this wastes ~25ms/ledger. Caching the uint256 keyHash in a member
variable eliminates these redundant SHA-256 computations.

## Change Summary
- Added `uint256 mCachedKeyHash` member to `ValueEntry` in `InMemorySorobanState.h`
- Initialized it in the constructor via `getTTLKey(LedgerEntryKey(*entry.ledgerEntry)).ttl().keyHash`
- Changed `copyKey()` to return `mCachedKeyHash` directly
- Changed `hash()` to use `mCachedKeyHash` directly

## Results

### TPS
- Baseline: 17,984 TPS
- Post-change: 17,792 TPS
- Delta: -192 TPS (-1.1%)

### Tracy Analysis
- `updateState` self-time: 400ms / 4 calls = 100ms/call (baseline: 355ms / 4 = 88.7ms/call)
- `applyLedger` avg: 1,028ms (baseline: 1,013ms)
- Regression of ~11ms/ledger in `updateState`

## Why It Failed

The cached hash adds 32 bytes (`uint256`) to each `ValueEntry`. The
`mContractDataEntries` unordered_set stores potentially hundreds of thousands of
entries (one per ContractData entry in the ledger). Adding 32 bytes per entry
significantly increases the memory footprint of the set, degrading CPU cache
locality during iteration and lookup operations.

The SHA-256 savings (~200ns per avoided recomputation) is offset by the cache
miss penalty from larger entries. The unordered_set's `hash()` during *lookup*
already uses `QueryKey::hash()` which was already efficient (just hashing the
uint256 directly). The `ValueEntry::hash()` is only called during *insertion*
(emplace), and `copyKey()` during equality comparison after hash collision.

With the map containing many entries, the memory layout change dominates over
the SHA-256 savings.

**Note**: Experiment 053 (a prior success) cached the TTL key hash for
`getTTLKey` *calls* throughout the codebase. This experiment attempted to extend
that approach to the unordered_set storage itself, but the memory-layout cost
outweighed the computation savings.

## Files Changed
- `src/ledger/InMemorySorobanState.h` — Added mCachedKeyHash to ValueEntry (reverted)
