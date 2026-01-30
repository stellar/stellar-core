---
name: high-level-code-review
description: reviewing a change for semantic correctness, simplicity, design consistency, and completeness
---

# Overview

This skill is for performing a high-level code review that requires
understanding the purpose and context of a change. Unlike low-level review
(which catches mechanical mistakes), high-level review evaluates whether the
change is correct in its intent and approach.

This skill involves a fair amount of work. Consider splitting it into pieces and
running each piece as a subagent (e.g., one subagent per review criterion, or
one per file/component). Keep each subagent focused on a moderate amount of work
so it doesn't get lost or wander off track.

The output is a **worklist** of issues to address, or confirmation that no
issues were found.

# Inputs

Before starting the review, gather the following information (if running as a
subagent, the invoking agent should provide these; otherwise, determine them
yourself or ask the user):

1. **Goal of the change**: What is the change trying to accomplish? This could
   come from an issue description, PR description, commit message, or user
   explanation. This is **required** for high-level review.

2. **Git range**: The git command to get the diff (e.g., `git diff master...HEAD`).

3. **Any specific concerns** (optional): Areas the user wants extra attention on.

If invoking as a subagent, the prompt should include: "Review the change for:
<goal>. Get the diff using `<git-command>`. <optional specific concerns>"

# Obtaining Context

Before reviewing, gather sufficient context:

1. **Understand the goal**: Use the goal description provided by the invoking
   agent.

2. **Get the diff**: Use the git command provided by the invoking agent.

3. **Build an understanding of context**. Use LSP tools to get lists of symbols
   and call graphs and type hierarchies as necessary. For each modified file,
   read enough of the surrounding code to understand the context (at minimum,
   the entire function or class being modified).   

4. **Find similar patterns**: Search the codebase for similar functionality to
   understand established patterns.

# Review Criteria

Evaluate the change against each of these criteria:

## Correctness

- Does the change accomplish its stated goal?
- Does it handle all the cases it claims to handle?
- Are the algorithms and logic correct?
- Are boundary conditions handled properly?
- Is arithmetic correct (especially with different integer types)?

## Completeness

- Are there edge cases not handled?
- Are error paths handled appropriately?
- If adding a feature, is it fully implemented or partially?
- Are there blocks of code missing with "TODO" or "FIXME" or
  "later" or "for now" or "the real version will do..."
- Are all code paths tested or testable?
- Is there sufficient debug logging?
- If you see TransactionFrame being touched, always check if
  FeeBumpTransactionFrame support was also added.

## Performance

- Are any algorithms of a higher complexity class than they should be? For
  example, are there quadratic algorithms where there should be linear or linear
  where there should be logarithmic or constant?
- Are appropriate types of containers being used in all cases?
- Are large data structures copied unnecessarily?
- Are copy constructors used where move constructors might work (without
  making the logic hard to read)?
- Are any long running blocking operations running on a thread that needs to
  remain responsive? For example, are there any unbounded IO operations on the
  main thread? Any slow cryptography?
- If it is reasonable to have metrics, are there metrics that track the behavior
  of the new code?
- If it is reasonable to have Tracy's ZoneScoped annotations for tracing, are
  they present?

## Consistency

- Is the approach consistent with how similar things are done elsewhere in the
  codebase?
- Does it follow established patterns for this type of change?
- Are naming conventions followed?
- Is the code organization consistent with surrounding code?

## Safety

- Could the change have unintended side effects?
- Are invariants maintained?
- Is thread safety preserved (if applicable)?
- Is there any new risk of data inconsistency?
- Is there any new risk of non-determinism? is the global deterministic
  pseudo-random number generator used consistently anywhere where
  pseudo-non-determinism is desired?
- Are any containers unordered when they should be ordered, or vice versa?
- If there are unanticipated errors, does the code fail safely?
- Are resources properly managed (no leaks, no use-after-free)?
- Is input validation sufficient?

## Error Handling

- Are errors detected and reported appropriately?
- Are error messages clear and actionable?
- Is error recovery handled correctly?
- Are exceptions used appropriately (if at all)?

## Minimality

- Is the change minimal, or does it include unnecessary modifications?
- Are there changes that should be split into separate commits/PRs?
- Is there dead code or debugging code that should be removed?
- Does the change add files it doesn't need to add?
- Does it add classes or functions that it doesn't need to add?
- Is anything duplicated that shouldn't be duplicated?
- Does it add unnecessary wrappers or layers of indirection?
- Does it add unnecessary backwards compatibility code?

## Documentation

- Are complex algorithms or non-obvious code explained in comments?
- Are public APIs documented?
- Do comments accurately reflect what the code does?
- Are the comments too verbose? Do they detract from clarity?
- Are any documentation files (README, docs/ etc.) updated if needed?

# Output Format

Produce a structured report as output. For each issue found:

1. **File path** and **line number(s)**
2. **Category** (from the criteria above)
3. **Severity**: Critical / Major / Minor / Suggestion
4. **Description** of the issue
5. **Recommendation** for how to address it

Example format:

```
## Issues Found

### src/ledger/LedgerManager.cpp:142-150 — Completeness (Major)
**Issue:** The new `processTransaction` path does not handle the case where
the transaction has no operations.
**Recommendation:** Add a check for empty operations and return an appropriate
error code, similar to how `processPayment` handles this at line 89.

### src/ledger/LedgerManager.cpp:200 — Consistency (Minor)
**Issue:** Variable named `txResult` but similar variables elsewhere use
`transactionResult`.
**Recommendation:** Rename to `transactionResult` for consistency.
```

If no issues are found:

```
## No Issues Found

The change appears correct and complete. Observations:
- [Any positive observations or notes for the record]
```

# ALWAYS

- ALWAYS understand the stated goal before reviewing
- ALWAYS read enough context to understand what the code is doing
- ALWAYS check for similar patterns in the codebase before flagging inconsistency
- ALWAYS distinguish between "definitely wrong" and "could be improved"
- ALWAYS provide specific, actionable recommendations
- ALWAYS cite file and line numbers
- ALWAYS consider whether apparent issues might be intentional
- ALWAYS prioritize correctness issues over style issues

# NEVER

- NEVER review without understanding what the change is trying to do
- NEVER flag style issues that are consistent with surrounding code
- NEVER suggest refactoring unrelated code
- NEVER recommend changes outside the scope of the current work
- NEVER assume something is wrong just because you would do it differently
- NEVER report issues without a concrete recommendation
- NEVER conflate personal preference with objective problems

# Completion

Summarize your work as follows:

1. Summary: number of issues by severity
2. The detailed issue list (or confirmation of no issues)
3. Overall assessment: Ready to proceed / Needs attention / Major concerns

If invoked as a subagent, pass this summary back to the invoking agent.
