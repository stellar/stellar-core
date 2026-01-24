---
name: low-level-code-review
description: reviewing a git diff for small localized coding mistakes that can be fixed without high-level understanding
---

# Overview

This skill is for performing a low-level code review on a git diff, looking for
small, localized coding mistakes that can be identified and fixed without any
high-level understanding of the codebase. These are the kinds of mistakes that
could occur in any codebase, in any language, and are purely mechanical in
nature.

For larger diffs, consider splitting the review into pieces and running each
piece as a subagent (e.g., one subagent per file or directory). Keep each
subagent focused on a moderate amount of work so it doesn't get lost or wander
off track.

The output is a **worklist** of issues to fix.

# Inputs

Before starting the review, gather the following information (if running as a
subagent, the invoking agent should provide these; otherwise, determine them
yourself or ask the user):

1. **Git range**: Determine which diff to review:
   - Check if there are uncommitted changes (`git diff` and `git diff --cached`)
   - If no uncommitted changes, check if current branch differs from `master`
   - If neither applies, ask the user for a specific git range

2. **Context about the change** (optional but helpful): A brief description of
   what the change is intended to do, if known.

If invoking as a subagent, the prompt should include: "Review the diff from
`<git-command>`. <optional context>"

# Obtaining the Diff

Run the git diff command provided by the invoking agent to obtain the diff,
then analyze it.

# Issues to Look For

Focus only on issues that are clearly mistakes and can be fixed with confidence.
Do not flag anything that requires understanding the broader system design.

## Definite Bugs

- **Numeric overflow/underflow**: Operations on integer types that could exceed
  their bounds (e.g., adding two `uint32_t` values near max).
- **Off-by-one errors**: Loop bounds, array indices, range calculations.
- **Null/nullptr dereference risk**: Dereferencing a pointer without checking if
  it could be null, especially after operations that might return null.
- **Uninitialized variables**: Variables used before being assigned a value.
- **Resource leaks**: Memory, file handles, or other resources acquired but not
  released on all code paths.
- **Use-after-free/move**: Using a resource after it has been freed or moved.
- **Double-free**: Freeing or deleting a resource twice.
- **Boolean logic errors**: Wrong operator precedence, De Morgan's law mistakes,
  inverted conditions.
- **String formatting mismatches**: Format specifier doesn't match argument type
  (e.g., `%d` for a `size_t`).

## ACID-semantics violations (on disk)

- Look for write-to-temp + rename patterns
- Verify temp file is fsynced before rename
- Verify directory is fsynced after rename
- Check: what happens if crash occurs between steps?
**Questions to ask:**
- If power is lost mid-operation, what state is on disk?
- Can the operation be resumed/retried after crash?
- Are there ordering dependencies between file writes?

## ACID-semantics violations (in memory)
- Look for non-atomic state changes that could leave the system in an inconsistent state if interrupted
- Check for proper locking around shared state
- Check for proper error handling that rolls back or compensates for partial failures
**Questions to ask:**
- If an exception is thrown or an error occurs mid-operation, could the system be left in an inconsistent state?
- Are state changes made atomically, or could they be interrupted leaving partial updates?
- Are there any critical sections that lack proper locking?

## Likely Mistakes

- **Copy-paste errors**: Duplicated code blocks with subtle inconsistencies that
  suggest a copy-paste where something wasn't updated.
- **Typos in identifiers**: Variable or function names that are almost but not
  quite right (especially in new code that mirrors existing patterns).
- **Wrong variable used**: Using a similarly-named variable by mistake.
- **Missing `break` in switch**: Fall-through that appears unintentional.
- **Comparison instead of assignment** (or vice versa): `if (x = y)` vs `if (x == y)`.
- **Signed/unsigned comparison**: Comparing signed and unsigned integers in ways
  that could produce unexpected results.
- **Use of raw pointers**: Use smart pointers, optionals or references.
- **Manual cleanup paths**: Use RAII guards.
- **Common C++ stdlib misuse**: Using the wrong container, algorithm, or utility
  function for a task, calling container methods or constructors incorrectly,
  introducing known UB, performance antipatterns, etc.

## Style Issues (Only if Clearly Wrong)

- **Typos in comments**: Misspelled words in comments or documentation.
- **Inconsistent naming**: New code that doesn't follow the naming pattern of
  immediately surrounding code.
- **Missing `const`**: Parameters or variables that could clearly be const but
  aren't.
- **Unused variables**: Variables declared but never used.
- **Unused includes**: Headers included but nothing from them appears to be used
  in the changed code.
- **Dead code**: Code that can never execute (after unconditional return, etc.).
- **West const**: const should go to the right of the constant thing ("east const").
- **Old idioms**: We are on C++20, so you should lean into features that help with
  correctness and clarity when appropriate.

# Output Format

Produce a structured worklist as output. Each item should contain:

1. **File path** and **line number** (from the diff)
2. **Issue type** (from the categories above)
3. **Original code** (the exact problematic code)
4. **Suggested fix** (the specific edit to make)
5. **Brief explanation** (why this is a problem, one sentence)

Group issues by file. Example format:

```
## src/foo/Bar.cpp

### Line 142: Numeric overflow risk
**Original:** `uint32_t total = count1 + count2;`
**Fix:** `uint64_t total = static_cast<uint64_t>(count1) + count2;`
**Why:** Both operands are uint32_t and their sum could exceed UINT32_MAX.

### Line 287: Typo in comment
**Original:** `// Calcualte the checksum`
**Fix:** `// Calculate the checksum`
**Why:** Misspelled "Calculate".
```

# ALWAYS

- ALWAYS cite the exact line number from the diff
- ALWAYS quote the original code exactly as it appears
- ALWAYS provide a specific, concrete fix (not just "fix this")
- ALWAYS explain why it's a problem in one sentence
- ALWAYS focus only on changed lines in the diff (lines starting with `+`)
- ALWAYS group issues by file for easier processing
- ALWAYS consider whether an issue might be intentional before reporting
- ALWAYS prioritize potential runtime errors over style issues
- ALWAYS check if a correctness condition is actually checked earlier in the same function before reporting
- ALWAYS verify the issue exists in the new code, not just the removed code

# NEVER

- NEVER change logic or behavior beyond the minimal fix required
- NEVER suggest refactoring, redesign, or architectural changes
- NEVER flag issues that require understanding the broader codebase or system design
- NEVER report style preferences that aren't clearly inconsistent with surrounding code
- NEVER suggest changes to lines that weren't modified in the diff
- NEVER make assumptions about programmer intent for ambiguous cases
- NEVER report the same mechanical issue more than 3 times; instead note "and N similar occurrences in this file"
- NEVER flag intentional patterns (e.g., don't flag `if (auto* p = getPtr())` as "assignment in condition")
- NEVER report issues in test code that are clearly intentional (e.g., testing error paths)
- NEVER spend time on issues that a compiler warning would catch (assume the build has warnings enabled)

# Completion

Summarize your work as follows:

- Present the worklist of issues found
- If no issues were found, report that explicitly: "No low-level issues found in
  the diff."
- If the diff is very large (more than ~500 lines of additions), suggest
  splitting the review by file or directory to ensure thoroughness.

If invoked as a subagent, pass this summary back to the invoking agent.
