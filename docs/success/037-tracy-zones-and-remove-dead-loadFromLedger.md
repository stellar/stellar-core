# Experiment 037: Add Diagnostic Tracy Zones + Remove Dead loadFromLedger

## Date
2026-02-23

## Hypothesis
1. Adding Tracy zones to the pre-parallel setup path (GlobalParallelApplyLedgerState
   constructor, preParallelApply loop, fetchClassicEntries loop) will give us
   detailed breakdown of the ~146ms sequential overhead before parallel apply.
2. Removing the dead `SorobanNetworkConfig::loadFromLedger(ltx)` call at
   LedgerManagerImpl.cpp:2956, which loads ~16 config settings but never uses
   the result, will save time in finalizeLedgerTxnChanges.

## Change Summary

### Tracy diagnostic zones added (ParallelApplyUtils.cpp):
- `ZoneScoped` on `GlobalParallelApplyLedgerState` constructor
- `ZoneNamedN("preParallelApply all txs")` around the sequential loop calling
  `preParallelApply` on all transactions
- `ZoneNamedN("fetchClassicEntries from footprints")` around the classic entry
  loading loop

### Dead code removal (LedgerManagerImpl.cpp):
- Removed `auto sorobanConfig = SorobanNetworkConfig::loadFromLedger(ltx)` at
  line 2956, which was loaded but the variable was never referenced before going
  out of scope at line 3030. The actual used config is `finalSorobanConfig`
  loaded at line 3037.

## Results

### TPS
- Baseline: 15,680 TPS [15,680 - 15,744]
- Post-change: 16,064 TPS [16,064 - 16,192]
- Delta: +2.4% / +384 TPS

### Tracy Analysis (per ledger, averaged over 3-4 samples)

| Zone | Baseline | Post-change | Delta |
|------|----------|-------------|-------|
| applyLedger | 1,189ms | 1,167ms | -22ms |
| applySorobanStages | 840ms | 819ms | -21ms |
| applySorobanStageClustersInParallel | 611ms | 598ms | -13ms |
| finalizeLedgerTxnChanges | 303ms* | 160ms | -143ms |
| commitChangesToLedgerTxn | 82ms* | 73ms | -9ms |

*Baseline values estimated from previous analysis without per-zone Tracy data.

### New Diagnostic Zone Breakdown (pre-parallel overhead)

| Zone | Time/ledger | Notes |
|------|------------|-------|
| GlobalParallelApplyLedgerState ctor | 136ms | Total constructor time |
| preParallelApply all txs | 122ms | Sequential loop: ~7.5us/TX * 16K TXs |
| fetchClassicEntries from footprints | 14ms | Minor |
| commitChangesToLedgerTxn | 73ms | Post-parallel commit |

The pre-parallel sequential overhead (136ms constructor + 73ms commit = 209ms)
represents ~18% of the applyLedger time. The biggest target is the 122ms
sequential `preParallelApply` loop.

## Files Changed
- `src/transactions/ParallelApplyUtils.cpp` -- Added Tracy zones to constructor
  and preParallelApplyAndCollectModifiedClassicEntries sub-loops
- `src/ledger/LedgerManagerImpl.cpp` -- Removed dead loadFromLedger call

## Commit
(see git log)
