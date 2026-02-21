# Experiment 028: Sequential Path Diagnostic Tracy Zones

## Date
2026-02-21

## Hypothesis
Adding comprehensive Tracy zones to all sequential operations in the ledger
close path (outside the parallel phase) would reveal the full per-ledger timing
breakdown and identify the next optimization targets.

## Change Summary
Added Tracy zones throughout the sequential path:

### LedgerManagerImpl.cpp
- `prefetchTxSourceIds` zone
- `prefetchTransactionData` zone
- `SorobanNetworkConfig::loadFromLedger` zone
- `buildTransactionBundles` zone
- `processPostTxSetApply` zone
- `xdrSha256 txResultSet` zone
- `processFeesSeqNums: commit` zone
- `finalize: resolveEviction` zone
- `finalize: getAllEntries` zone
- `finalize: addLiveBatch` zone
- `finalize: updateInMemorySorobanState` zone (async worker)
- `finalize: waitForInMemoryUpdate` zone
- `seal: snapshotLedger` zone
- `seal: storePersistentState` zone

### TransactionFrame.cpp (in commonPreApply)
- `computePreApplySorobanResourceFee` zone
- `commonValid` zone
- `processSeqNum` zone
- `processSignatures` zone
- `commonPreApply: pushAndCommit` zone

## Results

### TPS
- Baseline: ~14,784 TPS (previous)
- Post-change: 14,720 TPS [14720, 14784]
- Delta: ~0% (diagnostic zones only, no optimization)

### Per-Ledger Timing Breakdown (applyLedger = ~1260ms)

| Zone | Time (ms) | % of applyLedger |
|------|-----------|------------------|
| applySorobanStages | 882 | 70.0% |
| - preParallelApply | 186 | 14.8% |
| -- commonValid | 59 | 4.7% |
| -- processSignatures | 39 | 3.1% |
| -- processSeqNum | 14 | 1.1% |
| -- computePreApplySorobanResourceFee | 9 | 0.7% |
| -- pushAndCommit | 6.2 | 0.5% |
| -- OperationFrame::checkValid (unzoned) | 59 | 4.7% |
| - parallel phase + commit | ~696 | 55.2% |
| sealLedgerTxnAndStoreInBucketsAndDB | 183 | 14.5% |
| - resolveEviction | 46 | 3.7% |
| - getAllEntries | 20 | 1.6% |
| - addLiveBatch | 110 | 8.7% |
| - updateInMemorySorobanState (async) | 87 | - |
| processFeesSeqNums | 78 | 6.2% |
| processPostTxSetApply | 35 | 2.8% |
| buildTransactionBundles | 8 | 0.6% |
| xdrSha256 txResultSet | 1.9 | 0.2% |
| Other (prepareForApply, etc.) | 72 | 5.7% |

### Key Findings
1. The "unzoned 59ms" in preParallelApply is from `OperationFrame::checkValid`
   called at TransactionFrame.cpp:2052, which performs a completely redundant
   account load + trivial validation for SAC transfer TXs during apply.
2. Source account is loaded 4 times per TX: commonValidPreSeqNum, processSeqNum,
   removeOneTimeSignerFromAllSourceAccounts, OperationFrame::checkValid.
3. Signature verification cache (250K entries) appears to be working - most
   apply-time sig checks hit the cache.

## Files Changed
- `src/ledger/LedgerManagerImpl.cpp` — 14 diagnostic Tracy zones
- `src/transactions/TransactionFrame.cpp` — 5 diagnostic Tracy zones in commonPreApply

## Commit
(committed with exp-026 re-application)
