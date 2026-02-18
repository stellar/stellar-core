---
name: analyzing-tracy-profiles
description: analyzing Tracy profiler trace files (.tracy) using CLI tools for performance investigation
---

# Overview

This skill is for analyzing Tracy profiler trace files (`.tracy`) to investigate
performance issues in stellar-core. Tracy is an embedded profiling system that
instruments code with `ZoneScoped` and `ZoneName` macros to capture detailed
timing information at nanosecond resolution.

Tracy trace files are compressed binary files (LZ4 or Zstd) containing zone
events, messages, memory allocations, and other profiling data. This skill
focuses on **CLI-based analysis** using tools built within the stellar-core
repository, making it suitable for agentic workflows without requiring a GUI.

# Key Concepts

**Zones**: Named regions of code being profiled. Each zone has:
- Name (e.g., `applyLedger`, `recvTransaction`)  
- Source file and line number
- Total time (including children)
- Self time (excluding child zone time)
- Call count and statistics (min, max, mean, std dev)

**Self time vs Total time**: Total time includes time spent in child zones.
Self time is just the time in the zone itself, excluding children. Self time
is often more useful for identifying the actual code causing slowness.

# Benchmark-Specific Analysis

**Important**: When analyzing traces from benchmarks (e.g., `apply-load --mode
max-sac-tps`), the trace captures the **entire process**, including TX set
building, surge pricing, and other work that is NOT part of what the benchmark
actually measures. The benchmark only times specific zones (e.g., `applyLedger`
or `ledger.transaction.total-apply`).

**csvexport `-f` is a substring filter, not a hierarchical filter.** It matches
zone names containing the string, but does NOT limit results to children of that
zone. To understand what matters for a benchmark:

1. Identify the benchmark's measurement zone (e.g., `applyLedger` for max-sac-tps)
2. Look at that zone's **total time** to understand the benchmark envelope
3. Look at **self-time** of all zones, but only consider zones that are logically
4. Zones like `tryAdd`, `buildSurgePricedParallelSorobanPhase`, and
   `popTopTxs` are TX set construction — they dominate total trace time but are
   irrelevant to the apply-time benchmark measurement

For max-sac-tps specifically, the relevant top-level zone is `applyLedger`
(`ledger/LedgerManagerImpl.cpp`). Its key children include:
- `applyTransactions` / `applySorobanStages` / `applySorobanStageClustersInParallel`
- `parallelApply` (transaction/operation application)
- `InvokeHostFunctionOpFrame` (Soroban execution)
- `finalizeLedgerTxnChanges` / `commitChangesToLedgerTxn` (post-apply writes)
- Bucket operations (`getBucketEntry`, `InMemoryIndex scan`, etc.)

# Prerequisites

## Tool Locations

The Tracy CLI tools are built from source in the stellar-core repository:

| Tool | Source | Built Binary |
|------|--------|--------------|
| csvexport | `lib/tracy/csvexport/src/csvexport.cpp` | `lib/tracy/csvexport/build/unix/csvexport-release` |
| update | `lib/tracy/update/src/update.cpp` | `lib/tracy/update/build/unix/update-release` |
| capture | `lib/tracy/capture/src/capture.cpp` | Pre-built as `tracy-capture` in repo root |

## Building the Tools

If the tools aren't built, build them:

```bash
# Build csvexport (primary analysis tool)
make -C lib/tracy/csvexport/build/unix release \
  CC=gcc CXX=g++ TRACY_NO_ISA_EXTENSIONS=1 TRACY_NO_LTO=1 LEGACY=1

# Build update tool (for file manipulation)
make -C lib/tracy/update/build/unix release \
  CC=gcc CXX=g++ TRACY_NO_ISA_EXTENSIONS=1 TRACY_NO_LTO=1 LEGACY=1
```

## Tracy File Location

Tracy files are typically stored in `~/logs/tracy/` with descriptive names.
Files can be very large (100MB - 2GB+). They use LZ4 compression by default.

# Analysis Commands

## 1. Aggregate Zone Statistics (Default Mode)

Get summary statistics for all zones:

```bash
./lib/tracy/csvexport/build/unix/csvexport-release <trace.tracy>
```

**Output columns**:
| Column | Description |
|--------|-------------|
| name | Zone name |
| src_file | Source file path |
| src_line | Line number |
| total_ns | Total time in nanoseconds |
| total_perc | Percentage of total trace time |
| counts | Number of zone occurrences |
| mean_ns | Mean duration per call |
| min_ns | Minimum duration |
| max_ns | Maximum duration |
| std_ns | Standard deviation |

## 2. Self Time Analysis

Get zone self-times (excludes child zones) - often more useful:

```bash
./lib/tracy/csvexport/build/unix/csvexport-release -e <trace.tracy>
```

## 3. Individual Zone Events (Unwrap Mode)

Get every individual zone occurrence with timestamps:

```bash
./lib/tracy/csvexport/build/unix/csvexport-release -u <trace.tracy>
```

**Output columns**:
| Column | Description |
|--------|-------------|
| name | Zone name |
| src_file | Source file |
| src_line | Line number |
| ns_since_start | Timestamp from trace start |
| exec_time_ns | Duration of this occurrence |
| thread | Thread ID |

**Warning**: This can produce millions of rows for large traces!

## 4. Filtered Analysis

Filter zones by name substring:

```bash
# All bucket-related zones
./lib/tracy/csvexport/build/unix/csvexport-release -f "bucket" <trace.tracy>

# All transaction-related zones  
./lib/tracy/csvexport/build/unix/csvexport-release -f "transaction" <trace.tracy>

# Case-sensitive filter
./lib/tracy/csvexport/build/unix/csvexport-release -f "Apply" -c <trace.tracy>
```

## 5. Messages Only

Extract Tracy messages (if any were logged):

```bash
./lib/tracy/csvexport/build/unix/csvexport-release -m <trace.tracy>
```

# Common Analysis Patterns

## Finding Hotspots (Highest Total Time)

```bash
./lib/tracy/csvexport/build/unix/csvexport-release <trace.tracy> | \
  sort -t',' -k4 -rn | head -20
```

## Finding Self-Time Hotspots

More useful for finding actual code that's slow (not just callers of slow code):

```bash
./lib/tracy/csvexport/build/unix/csvexport-release -e <trace.tracy> | \
  sort -t',' -k4 -rn | head -20
```

## Finding Most Called Zones

```bash
./lib/tracy/csvexport/build/unix/csvexport-release <trace.tracy> | \
  sort -t',' -k6 -rn | head -20
```

## Finding High-Variance Zones

Zones with high std_ns relative to mean may indicate inconsistent performance:

```bash
./lib/tracy/csvexport/build/unix/csvexport-release <trace.tracy> | \
  awk -F',' 'NR>1 && $7>0 {print $10/$7, $0}' | sort -rn | head -20
```

## Comparing Two Traces

Use the existing `DiffTracyCSV.py` script to compare before/after traces:

```bash
# Export both traces in unwrap mode
./lib/tracy/csvexport/build/unix/csvexport-release -u old.tracy > old.csv
./lib/tracy/csvexport/build/unix/csvexport-release -u new.tracy > new.csv

# Run comparison
python3 scripts/DiffTracyCSV.py --old old.csv --new new.csv
```

The script reports zones with significant performance changes, showing:
- Median time change
- 90th percentile change  
- Total time change
- Event count change

# Trace File Manipulation

## Check Trace Info

```bash
# Quick size check
du -h <trace.tracy>

# Quick content check - count zones
./lib/tracy/csvexport/build/unix/csvexport-release <trace.tracy> | wc -l
```

## Recompress/Strip Data

Use the update tool to reduce file size or strip unnecessary data:

```bash
# Strip memory and sampling data, recompress with Zstd
./lib/tracy/update/build/unix/update-release -s Ms -z 10 input.tracy output.tracy
```

**Strip flags**:
- `l` = locks
- `m` = messages
- `p` = plots
- `M` = memory allocations
- `i` = frame images
- `c` = context switches
- `s` = sampling data
- `C` = symbol code
- `S` = source cache

# Performance Investigation Workflow

## Step 1: Quick Overview

Get aggregate stats and identify top time consumers:

```bash
./lib/tracy/csvexport/build/unix/csvexport-release <trace.tracy> | \
  sort -t',' -k4 -rn | head -30
```

## Step 2: Self-Time Analysis

Identify where time is actually spent (not inherited from children):

```bash
./lib/tracy/csvexport/build/unix/csvexport-release -e <trace.tracy> | \
  sort -t',' -k4 -rn | head -30
```

## Step 3: Focus on Specific Subsystem

Filter to the subsystem of interest:

```bash
./lib/tracy/csvexport/build/unix/csvexport-release -f "ledger" <trace.tracy>
./lib/tracy/csvexport/build/unix/csvexport-release -f "overlay" <trace.tracy>
./lib/tracy/csvexport/build/unix/csvexport-release -f "bucket" <trace.tracy>
./lib/tracy/csvexport/build/unix/csvexport-release -f "scp" <trace.tracy>
./lib/tracy/csvexport/build/unix/csvexport-release -f "herder" <trace.tracy>
```

## Step 4: Timeline Analysis (if needed)

For detailed timeline analysis, export individual events:

```bash
./lib/tracy/csvexport/build/unix/csvexport-release -u -f "applyLedger" <trace.tracy> > ledger_events.csv
```

Then analyze the CSV for patterns, gaps, or outliers.

## Step 5: Compare with Baseline

If you have a baseline trace, use DiffTracyCSV.py to detect regressions.

# ALWAYS

- ALWAYS use self-time (`-e` flag) when looking for actual code hotspots
- ALWAYS check tool is built before attempting analysis
- ALWAYS use filtering (`-f`) for large traces to focus on relevant zones
- ALWAYS use absolute paths for tracy files to avoid confusion
- ALWAYS pipe through `head` first to gauge output size before processing

# NEVER

- NEVER use unwrap mode (`-u`) without limiting output (can be millions of rows)
- NEVER report TX set building zones (tryAdd, buildSurgePricedParallelSorobanPhase, popTopTxs, applySurgePricing) as performance-relevant when analyzing apply-load benchmark traces — these are outside the measured benchmark window
- NEVER assume total time means the zone itself is slow (check self time)
- NEVER compare traces from different scenarios without noting the context
- NEVER delete or overwrite original tracy files

# Completion

After analysis, summarize:

1. **Top hotspots** identified (by total time and self time)
2. **Subsystem breakdown** - where time is distributed
3. **Anomalies** - high variance zones or unexpected patterns
4. **Comparison results** - if comparing traces, significant changes
5. **Recommended next steps** - what to investigate in code

If invoked as a subagent, pass this summary back to the invoking agent along
with any specific zone names and file locations that need investigation.

# Additional Resources

- Tracy documentation: `lib/tracy/manual/tracy.md`
- DiffTracyCSV script: `scripts/DiffTracyCSV.py`
- Performance evaluation guide: `performance-eval/performance-eval.md`
- Scripts README: `scripts/README.md`
