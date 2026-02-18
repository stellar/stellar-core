# How to Run the Ralph Optimization Loop

## Quick Start

```bash
# Start a tmux session (persists after SSH disconnect)
tmux new -s ralph

# Run the loop
export PATH="$HOME/.bun/bin:$PATH"
ralph --prompt-file ralph-prompt.md --agent opencode --max-iterations 20

# Detach from tmux (loop keeps running): Ctrl-b then d
```

**Important:** Do NOT pass `--model`. Without it, opencode uses your full
oh-my-opencode-slim dynamic preset — opus orchestrates while haiku explores,
codex-spark fixes, gpt-5.3 advises, and sonnet researches docs. Passing
`--model` may override the preset and restrict to a single model.

## Starting Headlessly

### Option A: tmux (recommended)

tmux lets you reattach later and see the full live output, including the
agent's tool calls and reasoning.

```bash
tmux new -s ralph -d \
  "export PATH=$HOME/.bun/bin:\$PATH && ralph --prompt-file ralph-prompt.md --agent opencode --max-iterations 20"
```

The `-d` flag starts it detached. The loop runs in the background immediately.

### Option B: nohup (fire-and-forget)

If you don't need to reattach to the live terminal:

```bash
export PATH="$HOME/.bun/bin:$PATH"
nohup ralph --prompt-file ralph-prompt.md --agent opencode --max-iterations 20 \
  > /mnt/xvdf/tracy/ralph-loop.log 2>&1 &
disown
```

Output goes to the log file. You lose interactive reattach but gain simplicity.

## Reconnecting

### tmux

```bash
# List sessions
tmux ls

# Reattach to the ralph session
tmux attach -t ralph

# Detach again without stopping: Ctrl-b then d
```

### nohup

```bash
# Watch the log live
tail -f /mnt/xvdf/tracy/ralph-loop.log
```

## Monitoring Progress

### Ralph built-in status (from any terminal)

```bash
export PATH="$HOME/.bun/bin:$PATH"

# Quick status: iteration count, elapsed time, struggle indicators
ralph --status

# Status including task list (if using --tasks mode)
ralph --status --tasks
```

This shows iteration history, which tools were used, duration per iteration,
and warnings if the agent appears stuck.

### Check experiment results

```bash
# See completed experiments
ls docs/success/ docs/fail/

# Read the latest experiment
cat docs/success/$(ls -t docs/success/ | head -1) 2>/dev/null
cat docs/fail/$(ls -t docs/fail/ | head -1) 2>/dev/null
```

### Check git log for committed improvements

```bash
git log --oneline -10
```

### Check if processes are running

```bash
# Is ralph running?
pgrep -fa ralph

# Is a benchmark or test currently executing?
pgrep -fa stellar-core
pgrep -fa "make check"
```

## Mid-Loop Guidance

If the agent is stuck or going in the wrong direction, you can inject hints
for the next iteration without stopping the loop:

```bash
export PATH="$HOME/.bun/bin:$PATH"

# Add a hint (consumed after one iteration)
ralph --add-context "Focus on reducing lock contention in applySorobanStageClustersInParallel"

# Clear pending hints
ralph --clear-context

# Or edit the context file directly
vim .ralph/ralph-context.md
```

## Stopping the Loop

### Gracefully (let current iteration finish)

```bash
# Send Ctrl-C if attached to the tmux session
# Ctrl-b then d to detach after

# Or from another terminal:
pkill -INT -f "ralph.*ralph-prompt"
```

### Immediately

```bash
pkill -9 -f "ralph.*ralph-prompt"
pkill -9 -f "opencode run"
pkill -9 -f "stellar-core apply-load"  # kill any running benchmark
```

### Clean up after stopping

```bash
# Check for stale processes
pgrep -fa stellar-core
pgrep -fa tracy-capture

# Kill if needed
pkill -9 -f stellar-core
pkill -9 -f tracy-capture

# Check for uncommitted changes from an interrupted experiment
git status
git diff --stat

# Revert if the interrupted experiment was incomplete
git checkout -- .
```

## Ralph State Files

Ralph stores iteration state in `.ralph/`:

| File | Purpose |
|------|---------|
| `.ralph/ralph-loop.state.json` | Active loop state (iteration, PID, prompt) |
| `.ralph/ralph-history.json` | Iteration history with timing and tool usage |
| `.ralph/ralph-context.md` | Pending hints for the next iteration |

These are created automatically. If you need to reset ralph's state:

```bash
rm -rf .ralph/
```

## Configuration

The prompt file is `ralph-prompt.md` in the repo root. Edit it to change what
the agent does each iteration. Key parameters:

| Flag | Current Value | Purpose |
|------|---------------|---------|
| `--max-iterations` | 20 | Safety limit on total iterations |
| `--agent` | opencode | Uses opencode with your oh-my-opencode-slim config |
| `--prompt-file` | ralph-prompt.md | The prompt sent each iteration |

### Adjusting iteration limits

```bash
# More iterations (overnight run)
ralph --prompt-file ralph-prompt.md --agent opencode --max-iterations 50

# Fewer iterations (quick test)
ralph --prompt-file ralph-prompt.md --agent opencode --max-iterations 5
```

### About `--model` (don't use it)

The `--model` flag forces a specific model on `opencode run -m <model>`. This
may override your oh-my-opencode-slim preset and disable subagent routing.

Your preset already configures:

| Role | Model | Purpose |
|------|-------|---------|
| orchestrator | claude-opus-4.6 | Main agent driving each iteration |
| explorer | claude-haiku-4.5 | Fast codebase search and pattern matching |
| fixer | gpt-5.3-codex-spark | Parallel implementation of well-defined tasks |
| oracle | gpt-5.3-codex | Deep architectural reasoning and debugging |
| librarian | claude-sonnet-4.6 | External docs lookup and library research |
| designer | gemini-3-pro-preview | UI/UX (not used in this workload) |

By omitting `--model`, all of these are available to the agent. The prompt
tells it to use `@explorer`, `@oracle`, `@fixer`, and `@librarian` subagents
for parallel analysis and implementation.
