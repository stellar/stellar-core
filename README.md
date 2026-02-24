# Autonomous AI Performance Optimization Experiment

This is a fork of [stellar-core](https://github.com/stellar/stellar-core) used as a testbed for **autonomous AI-driven incremental performance optimization**.

## Overview

The objective was to see whether an autonomous AI agent, running in a loop with no human guidance, could make meaningful, incremental performance improvements to stellar-core.

The specific target was the **SAC (Stellar Asset Contract) transfer TPS** benchmark (`apply-load --mode max-sac-tps`), which measures maximum sustainable throughput of the ledger apply path. The starting baseline was ~8,000 TPS, and so far the agent has gotten up to ~19,300 TPS.

Each iteration of the loop, the agent would: profile the code with Tracy, analyze hotspots, hypothesize an optimization, implement it, run tests, benchmark, and document the result — then repeat.

Commit 3506b1a715a0e8ba323cc69db3c68cd93cc0e17a was the first AI generated experiment. Commits before that were skills setup and stability improvements to the test itself. Commits c221a59fa27992928d5a9d3af4cc1c41627a70ae
through 25214fbcddef716609a3dc70644cd16e4f33fcfe were not made by the autonomous loop, but did involve some human interaction. This fixed an issue with commiting submodule changes and fixes to meta related unit tests.

## Results

Over the course of the experiment, the agent ran **52+ experiment cycles**, producing:

- **~45 successful optimizations** committed to the branch (see [`docs/success/`](docs/success/))
- **~50 failed experiments** documented locally (see [`docs/fail/`](docs/fail/))

Successful optimizations ranged from low-level changes (removing unnecessary Tracy zones from hot functions, adding move semantics to avoid XDR copies) to algorithmic improvements (sharded signature verification caches, parallel index construction, eliminating redundant child LedgerTxn allocations, skipping unnecessary validation during apply).

## How to Interpret These Results

Each successful commit should be viewed as a **very detailed issue** — a concrete, tested proposal that a human should cherry-pick, review, and decide whether it is valid and appropriate for production.

All unit tests pass on this branch, and a watcher node running on mainnet did not fork with these changes applied — up until metering/cost-model changes were introduced, which was an allowed protocol change exception given in the agent's prompt. Some "successful" experiments do appear to be flawed upon closer human review. However, real improvements were definitely realized by the agent that can be applied in production. The value is in the agent doing the legwork of profiling, hypothesizing, implementing, and testing — the human still needs to make the final call.

## Setup

### Skills

The agent was configured with a set of **custom skills** (reusable prompt documents in `.claude/skills/`) that taught it how to:

- Run the `max-sac-tps` benchmark with Tracy profiling
- Analyze Tracy trace files using CLI tools
- Build stellar-core correctly
- Run the test suite
- Execute the full optimization loop (profile, implement, test, benchmark, document)

These skills gave the agent the domain knowledge it needed to operate autonomously on an unfamiliar codebase without human hand-holding.

### The Ralph Loop

The [ralph](https://github.com/Th0rgal/open-ralph-wiggum) (open-ralph-wiggum) loop runner was experimented with for both agent systems. Ralph repeatedly invokes an AI coding agent with a fixed prompt, giving each iteration a fresh context window. The agent relies on file-system artifacts (experiment docs, git history, Tracy profiles) for continuity between iterations.

The prompt sent each iteration is in [`ralph-prompt.md`](ralph-prompt.md). It instructs the agent to:

1. Read all previous experiment docs to understand what's been tried
2. Run parallel discovery agents (Tracy analysis, code exploration, prior experiment review)
3. Run parallel solution exploration agents to evaluate competing optimization ideas
4. Pick the best candidate, implement it, test, benchmark, and document

The ralph loop was necessary for opencode to continue making good progress — without periodic context resets, quality degraded over long sessions. However, Claude Code was able to continue making good progress despite context compaction by simply running the `optimizing-max-sac-tps` skill standalone in a single long-lived session, without the ralph loop. This suggests Claude Code's context compaction handles this workload well enough that explicit iteration boundaries are not required.

### How to Run

See [`how-to-run.md`](how-to-run.md) for overly simplified AI generated instructions on starting, monitoring, and stopping the ralph optimization loop.

### Agent Configuration

Two agent systems were used interchangeably:

1. **[opencode](https://github.com/nicholasgriffintn/opencode)** with the [oh-my-opencode-slim](https://github.com/alvinunreal/oh-my-opencode-slim) plugin, which provides a multi-model "dynamic" preset. Rather than using a single model, oh-my-opencode-slim assigns specialized roles to different models and routes work to the best model for each task:

   | Role | Model | Purpose |
   |------|-------|---------|
   | **Orchestrator** | Claude Opus 4.6 | Main agent driving each iteration — reads skills, makes decisions, coordinates subagents |
   | **Explorer** | Claude Haiku 4.5 | Fast, cheap codebase search and pattern matching (finding files, grepping code) |
   | **Fixer** | GPT-5.3 Codex Spark | Parallel implementation of well-defined, scoped code changes |
   | **Oracle** | GPT-5.3 Codex | Deep architectural reasoning, complex debugging, and optimization strategy |
   | **Librarian** | Claude Sonnet 4.6 | External documentation lookup and library research (with web search, Context7, and grep.app MCPs) |
   | **Artist** | Gemini 3 Pro Preview | Creative review — a more hallucinogenic version of the Oracle, injecting unconventional optimization ideas |

2. **[Claude Code](https://docs.anthropic.com/en/docs/claude-code)** (Anthropic's CLI) with **experimental agent teams** enabled, running Claude Opus 4.6.

There did not seem to be a strong difference in performance between the two systems, so long as Claude Code had experimental agent teams enabled. Without agent teams, opencode performed better — which points towards the ability to spawn multiple parallel subagents being very helpful for this kind of task.

All permissions were set to `allow` in both systems so the agent could operate fully autonomously without human approval prompts.

## Recommendation

Initially, this experiment was run with Claude Code using Opus 4.5, before agent teams were available. In that setup, opencode with a ralph loop and multi-agent orchestration was the clear winner — single-agent Claude Code couldn't keep up.

After redoing the experiment with Claude Code using Opus 4.6 with experimental agent teams enabled, both systems seem to perform equally well. I'll continue experimenting to see if opencode's mixed-model routing can give better results, but for folks starting out, I'd recommend **Claude Code with agent teams** due to its simplicity — no plugins, no preset configuration, just enable the feature flag and go.

Once the prompt and skills were sufficiently setup, running the optimization loop with claude code is straightforward. After starting a session via `claude --dangerously-skip-permissions ` and
enabling "no permissions" mode, the entire prompt I inputted was just "/optimizing-max-sac-tps". Claude was able to run autonomously and make progress for days, simply running this skill. I would
strongly recommend building a robust skill set and a "memoized," long running skill like "optimizing-max-sac-tps." At this time I would recommend against investing time in exotic harnesses, plugins, or agentic orchestration.

