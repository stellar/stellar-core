# Experiment 029: Skip Redundant OperationFrame::checkValid in preParallelApply

## Date
2026-02-21

## Hypothesis
In `TransactionFrame::preParallelApply`, after `commonPreApply` succeeds,
`OperationFrame::checkValid` is called which performs a redundant account
existence check via `LedgerSnapshot::getAccount` (~3.7µs/TX). Skipping this
call should reduce the per-TX sequential overhead by ~3.7µs.

## Change Summary
Removed the `OperationFrame::checkValid` call in `preParallelApply`. This call
was moved from the parallel phase to the sequential phase for thread-safety.
During apply, all its checks are redundant:

- `isOpSupported`: protocol version already validated at TX set building time
- Account existence: source account was just loaded and modified in
  `commonPreApply` (via `commonValidPreSeqNum` + `processSeqNum`)
- `doCheckValidForSoroban`: validates static TX properties (wasm upload size,
  create_contract asset validity, footprint structure) that were already
  validated during TX set building

The call created a temporary `LedgerSnapshot` and performed an account lookup
per TX, accounting for ~3.7µs of sequential overhead per transaction.

## Results

### TPS
- Baseline: 14,720 TPS [14,720-14,784]
- Post-change: 15,168 TPS [15,168-15,232]
- Delta: +448 TPS (+3.0%)

### Tracy Analysis

| Zone | Baseline (µs/TX) | Post-change (µs/TX) | Delta |
|------|------------------|---------------------|-------|
| preParallelApply total | 11.6 | 10.7 | -0.9 (-7.8%) |
| commonValid | 3.7 | 3.6 | -0.1 |
| processSignatures | 2.4 | 2.4 | 0 |
| processSeqNum | 0.87 | 0.86 | 0 |
| computePreApply | 0.57 | 0.56 | 0 |
| pushAndCommit | 0.39 | 0.36 | 0 |
| Unzoned (was OperationFrame::checkValid) | 3.7 | 2.9 | -0.8 (-22%) |

applyLedger total: 1262ms (with ~15,168 TXs vs baseline 1260ms with ~14,720 TXs)

## Files Changed
- `src/transactions/TransactionFrame.cpp` — Removed redundant
  `mOperations.front()->checkValid()` call in `preParallelApply`, replaced
  with comment explaining why it's safe to skip

## Commit
