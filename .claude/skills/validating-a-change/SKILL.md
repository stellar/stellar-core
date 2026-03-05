---
name: validating-a-change
description: comprehensive validation of a change to ensure it is correct and ready for a pull request
---

# Overview

This skill is for validating that a change is correct, complete, and ready for
a pull request. It orchestrates several other skills to systematically check
formatting, review code, ensure test coverage, and run tests.

This skill involves a lot of work. Consider splitting it into pieces and running
each step as a subagent. Keep each subagent focused on a moderate amount of work
so it doesn't get lost or wander off track.

**Critical principle**: Each step must fully resolve its own issues before
proceeding to the next step. There is no need to restart from the beginning
when issues are found—fix them in place and continue forward.

The ordering prioritizes:
1. Code reviews first (catches design and semantic issues early)
2. Adding any missing tests
3. Running tests 
4. Multiple configurations (catches config-specific issues)
5. Formatting last (mechanical cleanup)

# Inputs

Before starting validation, gather the following information (if running as a
subagent, the invoking agent should provide these; otherwise, determine them
yourself or ask the user):

1. **Goal of the change**: What is the change trying to accomplish? This is
   needed for high-level code review.

2. **Type of change**: Is this a new feature, bug fix, refactor, or performance
   change? This is needed for the adding-tests step.

3. **Bug/issue reference** (if applicable): For bug fixes, the issue number.

4. **Any specific concerns** (optional): Areas wanting extra attention.

If invoking as a subagent, the prompt should include: "Validate change for:
<goal>. Change type: <type>. <issue reference if applicable> <specific concerns
if any>"

# Prerequisites

This skill composes several other skills:
- `low-level-code-review` - for mechanical code review
- `high-level-code-review` - for semantic code review
- `adding-tests` - for analyzing and adding test coverage
- `running-tests` - for running tests at all levels
- `running-make-to-build` - for building correctly
- `configuring-the-build` - for setting up different configurations

# Validation Steps

Execute these steps in order. Each step should fully resolve any issues it
finds before proceeding to the next step.

## Step 1: High-Level Code Review

Consider running as a subagent using the `high-level-code-review` skill.

Before launching, determine the git range:
- If uncommitted changes exist, use `git diff` and `git diff --cached`
- Otherwise, use `git diff master...HEAD`

Launch with prompt: "Review the change for: <goal from inputs>. Get the diff
using `<git-command>`. <specific concerns if provided>"

This reviews the change for semantic correctness, design consistency, and
completeness. The subagent will return a report of issues by severity.

If any Critical or Major issues are found, run a subagent to fix them before
proceeding. Minor issues and Suggestions can be addressed or deferred at your
discretion.

## Step 2: Low-Level Code Review

Consider running as a subagent using the `low-level-code-review` skill.

Launch with prompt: "Review the diff from `<git-command>`"

This reviews the diff for small, mechanical coding mistakes that don't require
high-level understanding. The subagent will return a worklist of issues.

If any issues are found, run a subagent to fix them before proceeding.

## Step 3: Add Tests

Consider running as a subagent using the `adding-tests` skill.

Launch with prompt: "Analyze and add tests for: <change type from inputs>.
Get the diff using `<git-command>`. <issue reference if applicable>"

This analyzes the change to determine what tests are needed and adds them.
The subagent will return a report of tests added.

## Step 4: Run Tests

Consider running as a subagent using the `running-tests` skill.

Before launching, identify the changed files/modules from the diff.

Launch with prompt: "Run tests through full suite for changes in <files/modules>."

This runs tests at levels 1-3: smoke tests, focused tests, and the full unit
test suite. The subagent will return a detailed report of results.

If any tests fail, run a subagent to fix the issues before proceeding.

## Step 5: Build and Test with Multiple Configurations

Consider running as a subagent using the `running-tests` skill with extended levels.

Launch with prompt: "Run tests through sanitizers for changes in <files/modules>.
Include fuzz tests if protocol-critical code was changed."

This runs tests at levels 4-6: sanitizer builds (ASan, TSan, UBSan), extra
checks build, and randomized/fuzz tests. See the `configuring-the-build` skill
for details on configuration options.

Also verify the build succeeds with `--disable-tests` (the production config).

If any configuration fails to build or any tests fail, run a subagent to fix
the issues before proceeding.

## Step 6: Format the Code

Run `make format` to auto-format all source code and verify formatting is clean.

# ALWAYS

- ALWAYS consider running long-running steps as subagents to keep them focused
- ALWAYS fully resolve issues within a step before proceeding to the next step
- ALWAYS wait for each subagent to complete before proceeding
- ALWAYS run high-level review before low-level review

# NEVER

- NEVER run multiple subagents in parallel (they may conflict)
- NEVER skip any step, even if you think it's unnecessary
- NEVER consider validation complete until all configurations pass
- NEVER skip high-level review just because you want to get to tests faster

# Completion

When all steps pass without requiring any code edits, the change is validated
and ready for a pull request. Summarize your work as follows:

1. Summary of what was validated
2. Configurations tested
3. Test coverage added (if any)
4. Any observations or minor concerns that didn't require changes

If validation cannot be completed (e.g., stuck in a loop), report:

1. Which step keeps failing
2. What issues keep recurring
3. Whether the fundamental approach may need reconsideration

If invoked as a subagent, pass this summary back to the invoking agent.
