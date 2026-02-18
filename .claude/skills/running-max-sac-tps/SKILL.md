---
name: running-max-sac-tps
description: running the max-sac-tps apply-load benchmark with Tracy profiling and analyzing results
---

# Overview

This skill covers running the `apply-load --mode max-sac-tps` benchmark, which
uses binary search to find the maximum sustainable SAC (Stellar Asset Contract)
transfer TPS. It also covers capturing Tracy profiles during the run and
interpreting the results.

The benchmark measures only the `applyLedger` zone time (or optionally including
DB writes via `APPLY_LOAD_TIME_WRITES`). TX set building, surge pricing, and
other overhead are NOT included in the measurement.

# Prerequisites

- stellar-core built with `--enable-tracy --enable-tracy-capture`
- The `tracy-capture` binary in the repo root
- The example config: `docs/apply-load-max-sac-tps.cfg`
- A directory for tracy output (e.g., `/mnt/xvdf/tracy/`)

# Running the Benchmark

## Step 1: Clean Up

Kill any stale stellar-core processes and free the Tracy port:

```bash
pkill -9 -f "stellar-core apply-load" 2>/dev/null
pkill -9 tracy-capture 2>/dev/null
sleep 2
# Verify port 8086 is free
ss -tlnp | grep 8086 || echo "Port 8086 clear"
rm -f stellar-core-apply-load.db 2>/dev/null
```

## Step 2: Start the Benchmark

Launch in background so we can attach Tracy:

```bash
./src/stellar-core apply-load \
  --conf docs/apply-load-max-sac-tps.cfg \
  --mode max-sac-tps \
  > /mnt/xvdf/tracy/benchmark-output.log 2>&1 &
BENCH_PID=$!
```

## Step 3: Attach Tracy Capture

**Wait ~30 seconds** for setup (account creation, contract deployment, upgrades)
profiling. Then capture a 30-second sample and let the benchmark continue:

```bash
sleep 30
./tracy-capture -o /mnt/xvdf/tracy/max-sac-tps.tracy -a 127.0.0.1 -f -s 30
```

The `-s 30` flag disconnects after 30 seconds. This keeps trace files manageable
(~1GB for 30s). The benchmark continues running after capture disconnects.

## Step 4: Wait for Results

```bash
wait $BENCH_PID
```

The final TPS result is logged via CLOG_WARNING to the Perf subsystem. Check
```
Maximum sustainable SAC payments per second: NNNN
```

Note: benchmark logs go through the logging system. If running without
`--console`, check the log file rather than stdout.

# Key Configuration Parameters

These are set in `docs/apply-load-max-sac-tps.cfg`:

| Parameter | Default | Description |
|---|---|---|
| `APPLY_LOAD_MAX_SAC_TPS_MIN_TPS` | 1000 | Lower bound for binary search |
| `APPLY_LOAD_MAX_SAC_TPS_MAX_TPS` | 15000 | Upper bound for binary search |
| `APPLY_LOAD_TARGET_CLOSE_TIME_MS` | 1000 | Target ledger close time |
| `APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS` | 4 | Parallel execution threads |
| `APPLY_LOAD_BATCH_SAC_COUNT` | 1 | SAC invocations per TX |
| `APPLY_LOAD_NUM_LEDGERS` | 20 | Ledgers per binary search iteration |
| `APPLY_LOAD_TIME_WRITES` | true | Include DB writes in timing |

## Automatically Set by CommandLine.cpp

The `max-sac-tps` mode automatically configures:

- `PARALLEL_LEDGER_APPLY = true` (parallel transaction execution)
- `RUN_STANDALONE = false` (required for parallel apply)
- `BUCKETLIST_DB_INDEX_PAGE_SIZE_EXPONENT = 0` (in-memory BucketList)
- `DISABLE_SOROBAN_METRICS_FOR_TESTING = true` (metrics are expensive at high load)
- `IGNORE_MESSAGE_LIMITS_FOR_TESTING = true` (TX set may exceed byte limits)
- File-based SQLite DB if none configured (required for parallel apply)

# Analyzing Tracy Profiles

## What the Benchmark Measures

The benchmark times `applyLedger` (or `ledger.ledger.close` when
`APPLY_LOAD_TIME_WRITES=true`). Only zones under `applyLedger` are relevant
to the TPS result. Zones outside this (TX set building, surge pricing) execute
during the benchmark but are NOT part of the measurement.

## Relevant Zone Hierarchy

```
applyLedger (LedgerManagerImpl.cpp)
├── applyTransactions
│   ├── applySorobanStages
│   │   ├── applySorobanStage
│   │   │   └── applySorobanStageClustersInParallel
│   │   │       └── parallelApply (TransactionFrame.cpp)
│   │   │           └── parallelApply (OperationFrame.cpp)
│   │   │               └── InvokeHostFunctionOpFrame doApply
│   │   │                   └── invokeHostFunction → SAC transfer
│   │   └── commitChangesToLedgerTxn
│   └── applyParallelPhase
├── finalizeLedgerTxnChanges
├── sealLedgerTxnAndStoreInBucketsAndDB
└── bucket operations (getBucketEntry, InMemoryIndex scan, etc.)
```

## Irrelevant Zones (DO NOT report as bottlenecks)

These zones appear in traces but are NOT part of the benchmark measurement:
- `tryAdd` (ParallelTxSetBuilder.cpp) — TX set construction
- `buildSurgePricedParallelSorobanPhase` — surge pricing
- `popTopTxs` — TX selection
- `applySurgePricing` — pricing logic
- `getInvalidTxListWithErrors` — TX validation
- `checkValidWithOptionallyChargedFee` — fee checking

## Quick Analysis Commands

```bash
CSVEXPORT=./lib/tracy/csvexport/build/unix/csvexport-release

# Total time breakdown (top zones)
$CSVEXPORT trace.tracy | sort -t',' -k4 -rn | head -30

# Self-time hotspots (where CPU actually spends time)
$CSVEXPORT -e trace.tracy | sort -t',' -k4 -rn | head -30

# Bucket-specific analysis (verify in-memory BL is working)
$CSVEXPORT -e -f "bucket" trace.tracy | sort -t',' -k4 -rn

# Soroban execution breakdown
$CSVEXPORT -e -f "soroban" trace.tracy | sort -t',' -k4 -rn | head -20
```

# ALWAYS

- ALWAYS kill stale stellar-core processes before starting a new run
- ALWAYS wait ~30 seconds after starting the benchmark before attaching Tracy
- ALWAYS use `-s 30` (or similar short duration) with tracy-capture to keep file sizes manageable
- ALWAYS let the benchmark finish naturally after Tracy detaches to get the TPS result
- ALWAYS focus analysis on zones under `applyLedger`, not the full trace
- ALWAYS check that `applyLedger` total time is reasonable relative to trace duration

# NEVER

- NEVER start tracy-capture before the benchmark process — it must connect to a running process
- NEVER kill the benchmark after Tracy capture finishes — the TPS result comes at the end
- NEVER report TX set building zones as apply-time bottlenecks
- NEVER run tracy-capture for the full benchmark duration — traces will be multi-GB
- NEVER run the unit test ("basic MAX_SAC_TPS functionality") when you want the actual benchmark — use `apply-load --mode max-sac-tps`

# Completion

Report:
1. The final maximum TPS number from the benchmark output
2. Tracy analysis focused on `applyLedger` children only
3. Top self-time hotspots within the apply path
4. Bucket/BL performance (should be negligible with in-memory BL)
5. Any anomalies or unexpected patterns
