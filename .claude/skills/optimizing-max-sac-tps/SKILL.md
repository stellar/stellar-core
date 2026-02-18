---
name: optimizing-max-sac-tps
description: autonomous optimization loop for maximizing SAC TPS benchmark results through creative performance improvements, testing, Tracy profiling, and experiment documentation
---

# Overview

This skill defines an autonomous optimization loop. The goal is to **maximize
the SAC (Stellar Asset Contract) transfer TPS** as measured by the `apply-load
--mode max-sac-tps` benchmark. The current baseline is ~8,000 TPS.
The target is **90,000+ TPS**.

You will loop indefinitely: profile → analyze → hypothesize → implement →
test → benchmark → document → repeat. Use subagents aggressively for parallel
analysis and research. Test ONE change at a time so you can attribute results.

# Prerequisites

Load these skills before starting:
- `running-max-sac-tps` — how to run the benchmark and capture Tracy profiles
- `analyzing-tracy-profiles` — how to analyze Tracy trace files
- `running-make-to-build` — how to build stellar-core
- `running-tests` — how to run tests

# The Optimization Loop

## Step 1: Review Previous Experiments

Before doing ANYTHING, read all files in `docs/success/` and `docs/fail/` to
understand what has already been tried. Do NOT repeat failed experiments unless
you have a fundamentally different approach. Deploy a subagent to summarize
previous experiments if the directories are large.

## Step 2: Run the Benchmark (Baseline)

Load the `running-max-sac-tps` skill and follow it exactly. Capture a 30-second
Tracy profile. Record:
- The TPS result (from log output)
- The Tracy trace file path
- A quick self-time analysis of the top 20 zones under `applyLedger`

If you already have a recent baseline from the current code state, skip this
step and use the existing baseline.

## Step 3: Analyze the Tracy Profile

Deploy subagents to analyze the Tracy trace in parallel:

1. **Self-time hotspots** — What zones have the highest self-time under
   `applyLedger`? These are the direct optimization targets.
2. **Parallelism efficiency** — Is `applySorobanStageClustersInParallel`
   effectively using all 4 threads? Look at per-thread zone timing with
   unwrap mode.
3. **Memory/allocation patterns** — Are there zones related to allocation,
   copying, or serialization that take significant time?
4. **Lock contention** — Look for zones that suggest serialization points
   (mutexes, shared state access).

## Step 4: Hypothesize an Optimization

Based on the analysis, form a hypothesis about what to optimize. Be creative.
Think about:

- **Algorithmic improvements**: Can hot loops be restructured?
- **Removing unnecessary work**: Are there checks, validations, or
  computations that can be skipped or deferred in the apply path?
- **Reducing allocations**: Can objects be reused or pooled?
- **Improving cache locality**: Can data layout be changed for better cache
  behavior?
- **Reducing lock contention**: Can shared state be partitioned?
- **Batching operations**: Can multiple small operations be combined?
- **Removing asserts in production code**: Asserts that are safe to remove
  (especially in hot paths) can help. Be careful about multithreaded races.
- **Compiler hints**: `[[likely]]`, `[[unlikely]]`, `__builtin_expect`,
  inlining hints.
- **Host/Rust-side optimizations**: The Soroban host functions run in Rust.
  After exhausting C++ optimizations, look at `soroban-env-host` code. No
  observable behavior changes, but implementation optimizations are OK.
- **Adding Tracy zones**: Before running a benchmark, add `ZoneScoped` to
  functions you want to measure. This helps identify sub-function hotspots
  in subsequent runs.

### Thinking Process

Deploy an @oracle subagent to deeply think about the optimization. Provide it
with:
- The Tracy analysis results
- The list of previous experiments (success and fail)
- The relevant source code sections
- The constraint list from this skill

Ask it to propose 2-3 ranked optimization ideas with expected impact estimates.

## Step 5: Implement the Change

Make the code change. Keep it focused — ONE optimization per experiment cycle.
If the change is large, break it into reviewable pieces but test the full
change as a unit.

### Adding Tracy Zones

If your optimization targets a function that doesn't have Tracy zones, **add
zones first** as a separate step. This way the NEXT benchmark run will give
you detailed timing for that function's internals.

```cpp
#include <tracy/Tracy.hpp>

void hotFunction() {
    ZoneScoped;  // Add this
    // ... existing code ...
}
```

## Step 6: Build and Test

### Build

Load the `running-make-to-build` skill. Build with:

```bash
make -j$(nproc)
```

If the build fails, fix the error and rebuild. Do NOT proceed to testing with
a broken build.

### Run Unit Tests

Run the transaction tests to verify correctness:

```bash
env NUM_PARTITIONS=30 TEST_SPEC="[soroban][tx]" make check
```

**This MUST pass before running the benchmark.** If tests fail:
1. Analyze the failure
2. Fix the code (your optimization, NOT the test)
3. Rebuild and retest

Do NOT change test logic. If your optimization changes an API, you may update
tests to use the new API, but the test assertions and coverage must remain
equivalent.

## Step 7: Run the Benchmark (Post-Change)

Run the benchmark exactly as in Step 2. Same configuration, same Tracy capture
duration. Record the same metrics.

## Step 8: Evaluate Results

Compare the post-change benchmark to the baseline:

| Metric | Baseline | Post-Change | Delta |
|--------|----------|-------------|-------|
| TPS | | | |
| applyLedger total time | | | |
| Top self-time zones | | | |

### Decision: Success or Failure?

**Success** = TPS improved OR Tracy metrics show meaningful improvement in the
apply path (even if TPS didn't change much due to variance — some improvements
compound).

**Failure** = TPS same or worse AND no meaningful Tracy metric improvement.

**Note on variance**: The benchmark has natural variance (~5-10%). If you see
a small improvement (< 5%), consider running the benchmark 2-3 times to
confirm the trend. A consistent 3% improvement across multiple runs is real.

## Step 9: Document the Experiment

### Successful Experiment

Create a file in `docs/success/NNN-short-description.md` where NNN is a
zero-padded sequence number. Include:

```markdown
# Experiment NNN: Short Description

## Date
YYYY-MM-DD

## Hypothesis
What you expected to improve and why.

## Change Summary
What code was changed (files, functions, approach).

## Results

### TPS
- Baseline: XXXX TPS
- Post-change: XXXX TPS
- Delta: +XX% / +XXXX TPS

### Tracy Analysis
- Key zone improvements (with numbers)
- Self-time changes for affected zones

## Files Changed
- `path/to/file.cpp` — description of change

## Commit
<commit hash after pushing>
```

Then **commit and push** the change along with the experiment doc:

```bash
git add -A
git commit -m "perf: <short description of optimization>"
git push
```

### Failed Experiment

Create a file in `docs/fail/NNN-short-description.md` with the same format,
but also include a **"Why It Failed"** section explaining your analysis of why
the optimization didn't work. This prevents repeating the same mistake.

**Do NOT commit failed experiments.** Revert the code change:

```bash
git checkout -- .
```

The fail doc stays locally as a reference for future iterations.

## Step 10: Repeat

Go back to Step 1. Re-read the experiment docs (new ones may exist if multiple
agents are running). Use the latest Tracy profile as the new baseline.

# Hard Rules

## NEVER

- NEVER change protocol semantics (cost model/metering changes ARE allowed)
- NEVER change the number of parallel threads (must be 4)
- NEVER change the batch size (must be 1 SAC invocation per TX)
- NEVER change the target close time (must be 1000ms)
- NEVER change the apply-load benchmark code itself (unless you find a specific
  bug in the benchmark measurement)
- NEVER look at or optimize `tryAdd`, `buildSurgePricedParallelSorobanPhase`,
  or anything outside the ledger apply path — these are NOT part of the
  benchmark measurement
- NEVER change unit test logic or delete tests (API-adaptation-only changes
  are allowed if your optimization changes an API)
- NEVER run the benchmark if unit tests don't pass
- NEVER run more than one stellar-core process at a time (benchmark OR test,
  not both)
- NEVER ask the user for advice — use your own judgment
- NEVER skip Tracy profiling — every benchmark run should capture a trace
- NEVER make multiple unrelated changes in one experiment cycle

## ALWAYS

- ALWAYS check `docs/success/` and `docs/fail/` before starting a new
  experiment to avoid repeating previous work
- ALWAYS test ONE change at a time to isolate its impact
- ALWAYS run `[tx]` tests before benchmarking
- ALWAYS capture Tracy profiles with every benchmark run
- ALWAYS document experiments (success in `docs/success/`, failure in
  `docs/fail/`)
- ALWAYS include both TPS results AND Tracy analysis in experiment docs
- ALWAYS revert failed experiments before starting the next one
- ALWAYS kill stale stellar-core processes before starting a benchmark
- ALWAYS start with small, targeted changes before attempting large redesigns
- ALWAYS add Tracy zones to functions you want to measure before benchmarking

# Subagent Deployment Strategy

Use subagents aggressively for parallel work:

- **@explorer**: Find relevant source code, locate hot functions, discover
  related implementations
- **@oracle**: Deep analysis of optimization opportunities, architectural
  advice, complex debugging
- **@fixer**: Implement well-specified code changes in parallel (e.g., adding
  Tracy zones to multiple files)
- **@librarian**: Research optimization techniques, compiler intrinsics,
  Rust performance patterns

Example parallel deployment:
1. @explorer: "Find all callers of X function"
2. @oracle: "Given these Tracy results, what are the top 3 optimization
   opportunities?"
3. @explorer: "Find the Rust implementation of Y host function"

Do NOT deploy subagents for trivial tasks (reading one file, making a
one-line change).

# Key Source Files

## C++ (stellar-core)

| File | Key Content |
|------|-------------|
| `src/ledger/LedgerManagerImpl.cpp` | `applyLedger` (line ~1459) — the measured zone |
| `src/ledger/LedgerTxn.cpp` | Ledger transaction management, commit paths |
| `src/transactions/TransactionFrame.cpp` | Transaction application, `parallelApply` |
| `src/transactions/OperationFrame.cpp` | Operation application |
| `src/transactions/InvokeHostFunctionOpFrame.cpp` | Soroban host function invocation |
| `src/simulation/ApplyLoad.cpp` | Benchmark implementation (DO NOT MODIFY) |
| `src/simulation/ApplyLoad.h` | Benchmark config (DO NOT MODIFY) |
| `src/main/Config.h` | Configuration parameters |
| `src/main/Config.cpp` | `allBucketsInMemory()`, `parallelLedgerClose()` |
| `src/bucket/` | Bucket and BucketList operations |

## Rust (soroban-env-host)

The Soroban host runs SAC transfer logic in Rust. For host-side optimizations:

| Path | Key Content |
|------|-------------|
| `src/rust/soroban/` | Rust bridge code |
| Soroban host repo | SAC transfer implementation, metering, storage |

Host-side changes must NOT change observable behavior — only implementation
optimizations (faster algorithms, less allocation, better cache usage).

# Configuration Reference

The benchmark uses these fixed parameters (from `docs/apply-load-max-sac-tps.cfg`
and `CommandLine.cpp`) DO NOT CHANGE THESE PARAMETERS!!:

| Parameter | Value | Notes |
|-----------|-------|-------|
| Threads | 4 | `APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS` |
| Batch size | 1 | `APPLY_LOAD_BATCH_SAC_COUNT` |
| Close time | 1000ms | `APPLY_LOAD_TARGET_CLOSE_TIME_MS` |
| BucketList | In-memory | `BUCKETLIST_DB_INDEX_PAGE_SIZE_EXPONENT = 0` |
| Parallel apply | true | `PARALLEL_LEDGER_APPLY` |
| Time writes | true | `APPLY_LOAD_TIME_WRITES` |
| Ledgers per iteration | 20 | `APPLY_LOAD_NUM_LEDGERS` |

# Tracy Profile Baseline (as of initial analysis)

Top self-time zones under `applyLedger` (30s sample):

| Zone | Self Time % | Notes |
|------|------------|-------|
| `applySorobanStageClustersInParallel` | 7.6% | Thread coordination overhead |
| `verify_ed25519_signature_dalek` | 5.6% | Signature verification (Rust) |
| Soroban budget `charge` | 3.0% | Metering overhead |
| `write xdr` | 2.8% | Serialization |
| Bucket operations | ~1% | Negligible with in-memory BL |

These percentages are relative to the full trace (not just applyLedger), so
within the apply path they represent larger fractions of the apply time.

# Completion

This skill runs as an indefinite loop. There is no "completion" — keep
optimizing until the TPS target is reached or the user intervenes.

After each experiment cycle, briefly report:
1. Experiment number and description
2. TPS result (baseline → post-change)
3. Success or failure
4. Next planned optimization

Then immediately start the next cycle.
