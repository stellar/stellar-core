# Experiment 067: Replace Spin-Wait with Blocking Wait in Parallel Apply

## Date
2026-02-24

## Hypothesis
`applySorobanStageClustersInParallel` has ~450ms self-time per ledger from a
tight spin-wait loop that polls futures with `wait_for(seconds(0))` + `yield()`.
Replacing the `yield()` with a short blocking wait (`wait_for(microseconds(100))`)
on the first uncommitted future would eliminate millions of wasted poll cycles
while still allowing out-of-order thread commit.

## Change Summary
In `LedgerManagerImpl::applySorobanStageClustersInParallel`, replaced the
`std::this_thread::yield()` fallback with a `wait_for(microseconds(100))`
blocking call on the first uncommitted future. The non-blocking sweep loop
remained unchanged — only the "no futures ready" fallback path was modified.

## Results

### TPS
- Baseline: 19,520 TPS (interval [305, 307])
- Post-change: 18,944 TPS (interval [296, 298])
- Delta: **-576 TPS (-3.0%)** — REGRESSION

### Tracy Analysis
- x=300 (19,200 TPS): mean close time 1010ms (was 1003ms in baseline)
- x=304 (19,456 TPS): mean close time 1010ms (was 993ms in baseline)
- High variance at both test points (variance ~1200-1850)

## Why It Failed
The 100-microsecond blocking wait introduces **commit latency**. When a worker
thread finishes, the main thread may be blocked inside `wait_for(100us)` on a
*different* future, delaying detection of the ready thread by up to 100us.
Over 4 threads × ~4 stages per ledger, these delays accumulate.

The original `yield()` loop, while CPU-wasteful, provides near-instant detection
of ready futures because it never truly blocks — it just yields the time slice
and immediately re-polls. On a machine with sufficient cores, the wasted CPU
from spinning does not contend with worker threads, so the spin-wait's latency
advantage outweighs its CPU waste.

### Key Insight
The 472ms self-time in `applySorobanStageClustersInParallel` is "wasted main
thread CPU" but does NOT hurt TPS because:
1. The machine has enough cores that the main thread doesn't steal from workers
2. The instant-detection property of the spin-wait is critical for minimizing
   commit latency and maximizing overlap with still-running threads
3. Even 100us of added commit latency compounds across stages to measurably
   regress TPS

This is a case where busy-waiting is the correct design choice.

## Files Changed (REVERTED)
- `src/ledger/LedgerManagerImpl.cpp` — replaced yield() with wait_for(100us)
