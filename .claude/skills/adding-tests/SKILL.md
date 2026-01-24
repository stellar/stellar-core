---
name: adding-tests
description: analyzing a change to determine what tests are needed and adding them to the test suite
---

# Overview

This skill is for analyzing a code change and adding appropriate test coverage.
It covers both unit tests and randomized/fuzz tests, ensuring that new
functionality is tested and bug fixes include regression tests.

This skill involves a fair amount of work. Consider splitting it into pieces and
running each piece as a subagent (e.g., one for analyzing test needs, one for
writing unit tests, one for randomized tests). Keep each subagent focused on a
moderate amount of work so it doesn't get lost or wander off track.

The output is either confirmation that tests were added (with details), or
confirmation that no additional tests are needed.

# Inputs

Before starting the analysis, gather the following information (if running as a
subagent, the invoking agent should provide these; otherwise, determine them
yourself or ask the user):

1. **Git range**: The git command to get the diff (e.g., `git diff master...HEAD`).

2. **Type of change**: Is this a new feature, bug fix, refactor, or performance
   change?

3. **Bug/issue reference** (if applicable): For bug fixes, the issue number or
   description of what was broken.

4. **Specific test requirements** (optional): Any specific testing requirements
   the user has mentioned.

If invoking as a subagent, the prompt should include: "Analyze and add tests
for: <change type>. Get the diff using `<git-command>`. <issue reference if
applicable> <specific requirements>"

# Analyzing Test Needs

## Step 1: Understand the Change

Get the diff using the command provided by the invoking agent:

Categorize the change:
- **New feature**: Adding new functionality
- **Bug fix**: Correcting incorrect behavior
- **Refactor**: Changing implementation without changing behavior
- **Performance**: Optimizing existing code

## Step 2: Find Existing Tests

Locate tests related to the changed code:

1. Look for test files with similar names (e.g., `Foo.cpp` → `FooTests.cpp`)
2. Search for tests that reference the modified functions/classes
3. Check if there are integration tests that exercise this code path

```bash
# Find test files
find src -name "*Tests.cpp" | xargs grep -l "FunctionName"
```

## Step 3: Determine What Tests Are Needed

### For New Features

- Unit tests for each new public function/method
- Tests for expected behavior with valid inputs
- Tests for edge cases (empty input, max values, etc.)
- Tests for error handling with invalid inputs
- Integration tests if the feature involves multiple components

### For Bug Fixes

- A regression test that would have failed before the fix
- The test should exercise the exact condition that caused the bug
- Include a comment referencing the issue/bug number if available

### For Refactors

- Existing tests should still pass (no new tests typically needed)
- If existing test coverage is inadequate, add tests before refactoring

### For Performance Changes

- Ensure functional tests still pass
- Consider adding benchmark tests if appropriate
- Consider adding metrics or tracy ZoneScoped annotations to help
  quantify performance

### Some Special Cases

- Write fee bump tests to go along with any new regular transaction tests or any
  logic changes to transaction processing and application.

## Step 4: Check for Randomized Test Needs

For changes affecting:
- Transaction processing
- Consensus/SCP
- Ledger state management
- Serialization/deserialization
- Any protocol-critical code

Consider whether randomized testing is appropriate:
- Fuzz targets for parsing/deserialization
- Property-based tests for invariants
- Simulation tests for distributed behavior

# Writing Tests

## Unit Test Patterns

Find existing tests in the same area and follow their patterns. Common patterns
in this codebase:

1. **Test fixture setup**: Look for how test fixtures are created
2. **Assertion style**: Match the assertion macros used elsewhere
3. **Test naming**: Follow the naming convention of nearby tests
4. **Helper functions**: Reuse existing test helpers rather than creating new ones

## Test File Organization

- Tests typically live in `src/` alongside the code they test, in a `test/`
  subdirectory.
- Test files are usually named `*Tests.cpp`
- There are often "test utility" helper files named `*TestUtils.cpp` 
- Tests are organized into test suites by component
- There are also some general testing utility files in `src/test`
- The unit test framework is "Catch2", a common C++ framework.

## Adding a Unit Test

1. Find the appropriate test file (or create one following conventions)
2. Add the test case following existing patterns
3. Ensure the test is self-contained and doesn't depend on external state
4. Run the new test in isolation to verify it works

## Adding a Fuzz Target

If adding a fuzz target:

1. Check existing fuzz targets in the codebase for patterns
2. Create a target that exercises the specific code path
3. Ensure the target can handle arbitrary input without crashing (except for
   intentional assertion failures)
4. Document what the fuzz target is testing

# Output Format

Report what was done:

```
## Tests Added

### Unit Tests

1. **src/ledger/LedgerManagerTests.cpp**: `processEmptyTransaction`
   - Tests that empty transactions are rejected with appropriate error
   - Regression test for issue #1234

2. **src/ledger/LedgerManagerTests.cpp**: `processTransactionWithMaxOps`
   - Tests boundary condition at maximum operation count

### Randomized Tests

1. **src/fuzz/FuzzTransactionFrame.cpp**: Extended to cover new transaction type
   - Added generation of the new transaction variant

## No Additional Tests Needed

[If applicable, explain why existing coverage is sufficient]
```

# ALWAYS

- ALWAYS find and follow existing test patterns in the same area
- ALWAYS include regression tests for bug fixes
- ALWAYS test both success and failure paths
- ALWAYS test edge cases and boundary conditions
- ALWAYS run new tests to verify they pass
- ALWAYS run new tests with the bug still present (if a regression test) to verify they would have caught it
- ALWAYS reuse existing test helpers and fixtures
- ALWAYS keep tests focused and independent

# NEVER

- NEVER add tests that depend on external state or ordering
- NEVER add tests that are flaky or timing-dependent
- NEVER duplicate existing test coverage
- NEVER write tests that test implementation details rather than behavior
- NEVER add tests without running them
- NEVER skip randomized test consideration for protocol-critical code
- NEVER create new test helpers when suitable ones exist

# Completion

Summarize your work as follows:

1. Summary of tests added (count and type)
2. Details of each test added
3. Confirmation that new tests pass
4. Any notes about test coverage that might still be lacking

If invoked as a subagent, pass this summary back to the invoking agent.
