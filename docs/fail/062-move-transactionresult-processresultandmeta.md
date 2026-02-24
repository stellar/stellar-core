# Experiment 062: Move TransactionResult in processResultAndMeta

## Date
2026-02-24

## Hypothesis
`processResultAndMeta` takes ~33ms/ledger serial (self-time 131ms/4 ledgers).
The function copies each TX's `TransactionResult` via `result.getXDR()` (const
reference → copy assignment) into a `TransactionResultPair`. With ~16K TXs per
ledger, eliminating the copy via `moveXDR()` (since result is not accessed
afterward) should save ~30ms.

## Change Summary
1. Added `TransactionResult moveXDR()` to `MutableTransactionResultBase`
2. Changed `processResultAndMeta` to take non-const result reference
3. Cached `isSuccess()` before move, used `result.moveXDR()` for zero-copy

## Results
- Baseline: 18,944 TPS
- Post-change: 18,944 TPS
- processResultAndMeta self-time: 131ms → 131ms (unchanged)

## Why It Failed
The TransactionResult for SAC transfers is tiny (~40 bytes): just an int64
feeCharged + xdr::xvector<OperationResult> with 1 element containing a 32-byte
hash. The copy cost per TX is ~60ns (1 heap alloc + 40-byte memcpy), totaling
only ~1ms for 16K TXs.

The 33ms self-time is dominated by:
- Tracy ZoneScoped overhead: ~13ms (200ns × 64K calls)
- Cache misses accessing scattered result/tx objects: ~10-15ms
- Metrics atomic increments: ~3-5ms
- Actual copy/move work: ~1-2ms

Move semantics save ~1ms at most — invisible against 32ms of overhead.

## Files Changed (reverted)
- `src/transactions/MutableTransactionResult.h` — Added moveXDR()
- `src/transactions/MutableTransactionResult.cpp` — Implemented moveXDR()
- `src/ledger/LedgerManagerImpl.h` — Changed signature to non-const
- `src/ledger/LedgerManagerImpl.cpp` — Used moveXDR(), cached isSuccess()
