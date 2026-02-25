# Experiment 074: Move XDR in processResultAndMeta (FAILED)

## Date: 2026-02-25

## Hypothesis
Replacing the deep copy of `TransactionResult` XDR with a move operation in
`processResultAndMeta` (no-meta path) would save ~2.2µs/TX × 16K TXs ≈ 35ms
per ledger of sequential overhead.

## Changes
1. Added `moveXDR()` method to `MutableTransactionResultBase` returning
   `TransactionResult&&` via `std::move(mTxResult)`
2. Changed `processResultAndMeta` signature from `const&` to `&` for the
   `result` parameter
3. In the no-meta path (benchmark mode), used `result.moveXDR()` instead of
   `result.getXDR()` to avoid the deep copy
4. Cached `result.isSuccess()` and `tx.isSoroban()` before the potential move

## First Run Failure
Initial implementation also gated metrics increments behind
`DISABLE_SOROBAN_METRICS_FOR_TESTING`. This caused a crash in
`setupUpgradeContract()` at ApplyLoad.cpp:702, which asserts
`mTxGenerator.getApplySorobanSuccess().count() == 2` — the setup phase
depends on those metrics even in benchmark mode. Fixed by removing the
metrics gating and keeping all counter increments unconditional.

## Result
- **Baseline**: 19,520 TPS
- **After**: 19,520 TPS
- **Change**: 0 TPS (no improvement)

## Analysis
The `TransactionResult` XDR for a SAC transfer is quite small (a few hundred
bytes at most), making the copy cost negligible. `processResultAndMeta` does
not even appear in the top 30 self-time zones in the Tracy profile. The
estimated 2.2µs/TX was an overestimate — the actual per-TX cost of the deep
copy is well below measurement threshold.

## Tracy Profile
- `/mnt/xvdf/tracy/max-sac-tps-074.tracy`

## Conclusion
The XDR result copy is not a meaningful bottleneck. The sequential overhead in
`processResultAndMeta` comes primarily from the `getContentsHash()` call and
`emplace_back` into the result vector, not from the XDR copy itself.

## Files Changed
- `src/transactions/MutableTransactionResult.h` — Added `moveXDR()` declaration
- `src/transactions/MutableTransactionResult.cpp` — Added `moveXDR()` implementation
- `src/ledger/LedgerManagerImpl.h` — Changed signature from `const&` to `&`
- `src/ledger/LedgerManagerImpl.cpp` — Used `moveXDR()` in no-meta path
