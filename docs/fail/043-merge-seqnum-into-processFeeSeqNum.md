# Experiment 043: Merge Sequence Number Bumping into processFeeSeqNum

## Date
2026-02-23

## Hypothesis
In `processFeesSeqNums`, every Soroban TX loads its source account to charge
fees via `processFeeSeqNum`. Later, in `preParallelApply` (inside
`GlobalParallelApplyLedgerState`), each TX loads the same source account
AGAIN just to bump the sequence number. By moving the seqNum bump into
`processFeeSeqNum` (where the account is already loaded), we can eliminate
~16K redundant account loads and save ~24ms from the 44ms
`GlobalParallelApplyLedgerState` cost.

## Change Summary
Modified `TransactionFrame::processFeeSeqNum` to also bump seqNum for V10+
Soroban TXs (and call `maybeUpdateAccountOnLedgerSeqUpdate`). Modified the
`preParallelApply` fast path (meta disabled) to skip `processSeqNum`.

## Results

### Build
- Compiled successfully

### Tests
- **FAILED**: `[soroban][tx]` tests fail with `postUpgradeCfg == upgradeCfg`
  assertion in `TestUtils.cpp:424`

## Why It Failed
The optimization is correct for the **parallel apply path** (where
`commonValidPreSeqNum` is skipped), but breaks the **sequential apply path**
used in tests (where `PARALLEL_LEDGER_APPLY=false`).

In the sequential path, `commonValidPreSeqNum` checks `accSeqNum >= getSeqNum()`.
If the seqNum is already bumped during fee processing, this check fails and
TXs are rejected. The SorobanTest constructor applies TXs (account creation,
contract deployment) via the sequential path, so those TXs get rejected.

### Why Not Fixed
Making the optimization conditional requires one of:
1. **Adding `bumpSeqNum` parameter to `processFeeSeqNum`**: Invasive — requires
   changing the pure virtual in `TransactionFrameBase`, overrides in
   `FeeBumpTransactionFrame` and `TransactionTestFrame`, plus all test callers.
2. **Doing the bump in `processFeesSeqNums` after `processFeeSeqNum` returns**:
   Requires re-loading the source account, negating the savings.
3. **Checking `parallelLedgerClose()` config flag**: Still requires a separate
   account load to do the bump.

All approaches either negate the savings or require disproportionate code churn
for a ~24ms (~2.2% of applyLedger) improvement.

## Files Changed (REVERTED)
- `src/transactions/TransactionFrame.cpp` — added seqNum bump in processFeeSeqNum,
  skipped processSeqNum in preParallelApply fast path
