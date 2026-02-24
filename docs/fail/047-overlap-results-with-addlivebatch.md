# Experiment 047: Overlap Result Set Building with addLiveBatch

## Date
2026-02-23

## Hypothesis
processResultAndMeta (~33ms/ledger) and xdrSha256 txResultSet (~2ms/ledger)
run sequentially on the main thread before finalizeLedgerTxnChanges. By
deferring these operations to run concurrently with addLiveBatch (108ms) during
finalization, we could save ~35ms per ledger (~3.3% improvement).

## Change Summary
Modified `applyTransactions` to accept an `outApplyStages` parameter. When the
caller passes a non-null pointer (indicating deferral is safe), the function
skips `processResultAndMeta` in `processPostTxSetApply` and instead moves the
`applyStages` out for later processing.

In `applyLedger`, a lambda was created to run `processResultAndMeta` +
`xdrSha256` concurrently with addLiveBatch via `std::async`. Also modified
`finalizeLedgerTxnChanges` to accept and dispatch a `concurrentWork` callback.
Moved `appendTransactionSet` to after the seal to handle deferred result sets.

## Results
- Tests: PASS (67 tests, 49,227 assertions)
- Benchmark: Two different crashes when the deferred path activates

### Failure 1: SIGSEGV (exit code 139)
When processResultAndMeta was called from the async thread (including metrics
access like `mApplyState.getMetrics().mSorobanTransactionApplySucceeded.inc()`),
the process crashed with SIGSEGV. Root cause: medida metrics counters are not
thread-safe for concurrent access.

### Failure 2: XDR Runtime Error
After removing metrics from the async lambda and inlining just the result
building logic (`txBundle.getResPayload().getXDR()`), the process crashed with
`std::runtime_error("bad value of code in _result_t")`. This indicates the
`TransactionResult` XDR discriminant was corrupt when serialized from the async
thread.

Root cause unclear but likely related to thread-unsafe access to
`MutableTransactionResultBase` objects. The `TxBundle` holds a raw reference
(`MutableTransactionResultBase&`) to result objects owned by `unique_ptr` in
`mutableTxResults`. While the data should be readable, something in the
concurrent execution context corrupts the read.

### Workaround Tested
Building the result set synchronously (on the main thread) and deferring only
the xdrSha256 hash computation to the async thread works without crashes. But
this only saves ~2ms (the hash time), not the full ~35ms target.

## Why It Failed
1. **Thread-unsafe metrics**: medida counters used in processResultAndMeta
   cannot be called from worker threads.
2. **Thread-unsafe result access**: Reading MutableTransactionResultBase::getXDR()
   from an async thread produces corrupt XDR data, despite the data being
   logically immutable at that point. The root cause is either a subtle data
   race in the XDR type (TransactionResult contains unions with discriminants),
   or a compiler optimization/memory ordering issue.
3. **Complex dependency chain**: The `TxBundle` stores a reference (not a copy)
   to the result object, creating a fragile ownership model that makes
   concurrent access risky.

## Files Changed (reverted)
- `src/ledger/LedgerManagerImpl.h`
- `src/ledger/LedgerManagerImpl.cpp`
- `src/bucket/test/BucketTestUtils.h`
- `src/bucket/test/BucketTestUtils.cpp`

## Lessons Learned
- Thread-safe concurrent access to TX results requires careful coordination.
  The existing `TxBundle` reference-based design is not suitable for deferred
  processing on worker threads.
- Any optimization that moves processResultAndMeta to a worker thread would
  need to either: (a) pre-copy the results to a thread-local buffer, or
  (b) redesign TxBundle to use value semantics / shared ownership.
- The ~33ms savings from overlapping result building with addLiveBatch is
  real but the implementation complexity is high for this approach.
