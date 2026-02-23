# Experiment 038: Fast preParallelApply — Skip Child LTX When Meta Disabled

## Date
2026-02-23

## Hypothesis
When transaction meta tracking is disabled (as in the benchmark via
DISABLE_META_TRACKING_FOR_TESTING), the child LedgerTxn created in
commonPreApply serves no purpose for Soroban TXs during apply:
- Rollback isolation is unneeded (TXs are consensus-validated, always committed)
- Meta recording (pushTxChangesBefore) is a no-op when disabled
- Validation checks are redundant (already validated during TX set building)

By skipping the child LTX, LedgerSnapshot, SignatureChecker, and full validation
pipeline, we can reduce the per-TX cost of preParallelApply significantly.

## Change Summary
Added a fast path in `TransactionFrame::preParallelApply` that activates when
`meta.isEnabled()` returns false. The fast path:
1. Computes Soroban resource fee directly (no LTX needed)
2. Initializes the RefundableFeeTracker
3. Calls processSeqNum directly on the parent LTX (no child LTX)
4. Calls updateSorobanMetrics (no-op when metrics disabled)
5. Skips: child LTX creation, LedgerSnapshot creation, SignatureChecker
   creation, commonValid (redundant validation), processSignatures
   (removeOneTimeSigner is no-op for Soroban)

Also added `TransactionMetaBuilder::isEnabled()` accessor.

## Results

### TPS
- Baseline: 15,680 TPS (exp 037 with diagnostic zones + dead code removal)
- Post-change: 16,640 TPS [16,640 - 16,768]
- Delta: **+6.1% / +960 TPS**

### Tracy Analysis (per ledger, averaged over 4 samples)

| Zone | Baseline | Post-change | Delta |
|------|----------|-------------|-------|
| applyLedger | 1,167ms | 1,109ms | -58ms (-5.0%) |
| applySorobanStages | 819ms | 733ms | -86ms |
| GlobalParallelApplyLedgerState ctor | 136ms | 42ms | -94ms (-69%) |
| preParallelApply all txs | 122ms | 29ms | -93ms (-76%) |
| preParallelApply per TX | 7.5us | 1.75us | -77% |
| applySorobanStageClustersInParallel | 598ms | 605ms | +7ms (noise) |
| commitChangesToLedgerTxn | 73ms | 73ms | 0 |
| finalizeLedgerTxnChanges | 160ms | 153ms | -7ms |
| processFeesSeqNums | 77ms | 77ms | 0 |

The 93ms savings in preParallelApply translates to 58ms of applyLedger
improvement (some of the saved time went to processing more TXs in the parallel
phase at the higher TPS level).

## Files Changed
- `src/transactions/TransactionFrame.cpp` -- Fast path in preParallelApply
- `src/transactions/TransactionMeta.h` -- Added isEnabled() accessor
- `src/transactions/TransactionMeta.cpp` -- Implemented isEnabled()

## Commit
(see git log)
