# Optimize SAC Transfer TPS — Single Experiment Cycle

You are one iteration of an optimization loop. Your job is to run exactly ONE
experiment to improve SAC transfer TPS, document the result, then signal
completion so the loop can restart you with fresh context.

## Context

The `apply-load --mode max-sac-tps` benchmark measures maximum sustainable SAC
(Stellar Asset Contract) transfer TPS. The target is **90,000+ TPS**. Previous
experiments are documented in `docs/success/` and `docs/fail/` — READ THESE
FIRST to understand what has been tried and what the current TPS baseline is.

## Your Task (One Experiment)

Load the `optimizing-max-sac-tps` skill, then load the prerequisite skills it
lists (`running-max-sac-tps`, `analyzing-tracy-profiles`, `running-make-to-build`,
`running-tests`).

Then do exactly ONE experiment cycle:

1. **Read all files in `docs/success/` and `docs/fail/`** — understand what
   was tried, what worked, what failed, and what the current baseline TPS is.
   DO NOT repeat failed experiments unless you have a fundamentally new approach.

2. **If no baseline exists yet**, run the benchmark with Tracy capture to
   establish one. Document the baseline TPS and Tracy analysis.

3. **Investigate using multiple agents in parallel.** Spin up agents to
   work simultaneously on two phases:

   **Phase A — Discovery (all agents run in parallel):**
   - **Agent 1 — Tracy profile analysis**: Analyze the most recent Tracy
     profile. Identify the top 5 self-time zones under `applyLedger`, wall-clock
     breakdown, and lock contention hotspots.
   - **Agent 2 — Code path exploration**: Explore the hot code paths identified
     in previous experiments. Search for redundant allocations, unnecessary
     copies, cache-unfriendly patterns, and missed parallelism opportunities.
   - **Agent 3 — Prior experiment review**: Read all docs in `docs/success/`
     and `docs/fail/`. Synthesize patterns: what categories of optimization
     tend to succeed vs fail? What remains untried? Identify the most promising
     unexplored direction.
   - **Agent 4 — Data structure & algorithm audit**: Examine the data
     structures and algorithms on the hot path (bucket operations, XDR
     serialization, hashing, map lookups). Look for algorithmic improvements
     or more cache-efficient alternatives.

   Wait for all discovery agents to return and collect their findings.

   **Phase B — Solution exploration (agents run in parallel):**
   Based on the discovery results, identify the top 3–4 most promising
   optimization ideas. Spin up one agent per idea to explore feasibility:

   - Each agent investigates ONE specific optimization candidate.
   - Each agent should: read the relevant code, sketch the change (do NOT
     apply it), estimate the expected impact, identify risks or blockers,
     and rate confidence (high/medium/low).
   - Agents should work independently — they are competing proposals.

   Wait for all solution agents to return.

4. **Pick ONE optimization** from the competing proposals. Prefer the one
   with the highest confidence, largest expected impact, and lowest risk.
   If multiple agents converged on the same bottleneck, that's a strong
   signal. Break ties toward simpler changes.

5. **Implement** the chosen change. Keep it focused — one optimization only.

6. **Build**: `make -j$(nproc)`

7. **Test**: `env NUM_PARTITIONS=20 TEST_SPEC="[tx]" make check`
   If tests fail, fix your change (not the tests). If unfixable, revert and
   document as failed.

8. **Benchmark** with Tracy capture. Compare TPS to baseline.

9. **Document the result**:
   - Success → `docs/success/NNN-short-description.md`, then `git add -A && git commit -m "perf: <description>" && git push`
   - Failure → `docs/fail/NNN-short-description.md`, then `git checkout -- .` (revert code, keep doc locally)

10. **Signal completion** by outputting the promise below.

## Hard Constraints (DO NOT VIOLATE)

- NO protocol changes (cost/metering changes OK)
- DO NOT change: thread count (4), batch size (1), target close time (1000ms)
- DO NOT change: apply-load benchmark code, unit test logic
- DO NOT optimize outside the ledger apply path (no tryAdd, no buildSurgePricedParallelSorobanPhase)
- DO NOT run benchmark if unit tests don't pass
- ONE change per experiment cycle
- `APPLY_LOAD_TIME_WRITES` must be `true`
- `APPLY_LOAD_NUM_LEDGERS` must be ≥ 10

## Environment

- Build: `--enable-tracy --enable-tracy-capture`, clang-20
- Tracy capture: `./tracy-capture`
- csvexport: `./lib/tracy/csvexport/build/unix/csvexport-release`
- Tracy output: `/mnt/xvdf/tracy/`
- Benchmark config: `docs/apply-load-max-sac-tps.cfg`
- Branch: `oh-my-opencode-test`

## Important: Keep Binary Search Range Tight

The benchmark config (`docs/apply-load-max-sac-tps.cfg`) has MIN_TPS and
MAX_TPS bounds for the binary search. Currently set to 7000–12000. If your
optimization pushes TPS near or above MAX_TPS, **raise MAX_TPS** in the config
before benchmarking so the search can find the true maximum. Keep the range
tight (within ~5000 of expected TPS) to minimize benchmark runtime.

## Completion

After documenting your experiment (success or failure), only if the TPS is at least 90,000, output:

<promise>INCOMPLETE</promise>
