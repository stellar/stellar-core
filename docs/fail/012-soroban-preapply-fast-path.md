# Experiment 012: Soroban Pre-Apply Fast Path

## Date
2026-02-20

## Hypothesis
The `preParallelApply` serial loop processes ~11K TXs on the main thread,
performing redundant work for Soroban TXs: `processSignatures` calls
`checkOperationSignatures` and `removeOneTimeSignerFromAllSourceAccounts`
(unnecessary for Soroban), and `checkValid` redundantly loads the source
account. Skipping this redundant work should save ~39ms/ledger (~3% of
apply time).

## Change Summary

1. **`src/transactions/TransactionFrame.cpp` — `processSignatures`**: Added
   early return for Soroban TXs with no separate operation source account.
   Skips `checkOperationSignatures` (redundant with TX-level check),
   `removeOneTimeSignerFromAllSourceAccounts` (Soroban never uses pre-auth
   signers), and `LedgerSnapshot` creation. Only keeps `checkAllSignaturesUsed`.

2. **`src/transactions/TransactionFrame.cpp` — `preParallelApply`**: When op
   has no separate source account, calls `checkValidForSorobanApply` instead
   of `OperationFrame::checkValid`. Skips redundant source account loading.

3. **`src/transactions/OperationFrame.cpp/h`**: Added `checkValidForSorobanApply`
   method that directly calls `doCheckValidForSoroban`, bypassing source
   account loading and signature checking.

## Results

### TPS
- Baseline: 14,144 TPS
- Run 1: 14,400 TPS (+1.8%)
- Run 2: 14,144 TPS (+0.0%)
- Average: ~14,272 TPS (+0.9%)

### Tracy Analysis
Tracy confirmed the code path changes were effective:
- `processSignatures`: 1.1ms/ledger (down from ~26ms) — **25ms saved**
- `checkValidForSorobanApply`: 0.16ms/ledger (down from ~14ms) — **14ms saved**
- `removeAccountSigner` and `checkOperationSignatures`: completely eliminated
- Total per-ledger savings: ~39ms

However, the ~39ms saved per ledger (~3% of 1,332ms) did not translate to
meaningful TPS improvement. The binary search granularity is 64 TPS, so
the smallest measurable improvement is ~0.5%. The savings may be real but
too small to consistently exceed benchmark variance.

## Why It Failed
The optimization saved real wall-clock time (~39ms/ledger confirmed by Tracy),
but this represents only ~3% of the total `applyLedger` time. The dominant
cost remains `applySorobanStageClustersInParallel` (65.7% of apply time),
which this change does not affect. The serial pre-apply loop is simply not
a large enough fraction of the total to produce a meaningful TPS delta.

Additionally, the TPS binary search has 64-TX granularity, meaning small
improvements can be masked by the search step size.

## Files Changed
- `src/transactions/TransactionFrame.cpp` — Soroban fast paths in `processSignatures` and `preParallelApply`
- `src/transactions/OperationFrame.cpp` — Added `checkValidForSorobanApply`
- `src/transactions/OperationFrame.h` — Declaration for `checkValidForSorobanApply`
