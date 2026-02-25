# Experiment 080: Hoist loadHeader out of per-TX processPostTxSetApply

## Hypothesis
In the no-meta benchmark path, `processPostTxSetApply` calls `processRefund` per TX, which calls `ltx.loadHeader()` each time. This creates and destroys a `LedgerTxnHeader` RAII handle per TX (~14K per ledger), involving `std::make_shared<Impl>()` heap allocation + weak_ptr construction/destruction + `deactivate()` call. Hoisting the `loadHeader()` call outside the TX loop should eliminate ~14K heap allocations per ledger.

## Change
- Added virtual `processPostTxSetApply` overload accepting `LedgerTxnHeader&` to `TransactionFrameBase`, `TransactionFrame`, `FeeBumpTransactionFrame`, and `TransactionTestFrame`
- Added `processRefund` overload accepting `LedgerTxnHeader&` to `TransactionFrame`
- Modified `LedgerManagerImpl::processPostTxSetApply` no-meta path to call `ltx.loadHeader()` once before the TX loop and pass the header to the new overload
- The existing `refundSorobanFeeWithHeader` already accepted `LedgerTxnHeader&`, so no changes were needed there

## Result
**FAILED** — No improvement, slight regression.

| Run | TPS | Range |
|-----|-----|-------|
| Baseline (exp 078) | 19,840 | [19,840, 19,904] |
| Run 1 | 19,264 | [19,264, 19,328] |
| Run 2 | 19,520 | [19,520, 19,648] |
| Average | 19,392 | -2.3% |

## Analysis
The `loadHeader()` per-TX cost is negligible despite involving a `make_shared` allocation:
- The `Impl` object is tiny (two references: `AbstractLedgerTxn&` + `LedgerHeader&`)
- Small allocations are very fast with modern allocators
- The overhead of adding a virtual method overload (vtable indirection, code duplication) may offset any savings
- The 50ms/ledger spent in `processPostTxSetApply` is dominated by `loadAccount()` (165ms total across both processFeesSeqNums and processPostTxSetApply) and `addBalance()`, not by `loadHeader()`

## Files Modified
- `src/transactions/TransactionFrameBase.h`
- `src/transactions/TransactionFrame.h`
- `src/transactions/TransactionFrame.cpp`
- `src/transactions/FeeBumpTransactionFrame.h`
- `src/transactions/FeeBumpTransactionFrame.cpp`
- `src/transactions/test/TransactionTestFrame.h`
- `src/transactions/test/TransactionTestFrame.cpp`
- `src/ledger/LedgerManagerImpl.cpp`

## Tracy Profile
`/mnt/xvdf/tracy/max-sac-tps-080.tracy`
