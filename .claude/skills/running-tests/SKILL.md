---
name: running-tests
description: running tests at various levels from smoke tests to full suite to randomized tests
---

# Overview

This skill is for running tests systematically, starting with fast/focused tests
and progressing to slower/broader tests. This ordering allows failures to be
caught early, minimizing wasted time.

This skill is designed to be run as a **subagent** to avoid cluttering the
invoking agent's context. The output is either confirmation that all tests
passed, or a report of failures.

# Required Inputs (Before Launching Subagent)

Since subagents cannot ask for clarification, the **invoking agent must gather
this information before launching**:

1. **Changed files/modules**: Which files or modules were changed, so the
   subagent can identify appropriate smoke tests and focused tests.

2. **Test levels to run**: Which levels to execute. Options:
   - "smoke only" - just Level 1
   - "through focused" - Levels 1-2
   - "through full suite" - Levels 1-3 (usually sufficient for small changes)
   - "through full suite with tx-meta" - Levels 1-3 plus tx-meta baseline check
   - "through sanitizers" - Levels 1-4 (for memory/concurrency-sensitive code)

The subagent prompt should include: "Run tests <levels> for changes in <files/modules>."

# Test Output Control

To reduce noise and keep agent context manageable, always use these flags:

```bash
# Recommended flags for quiet output
--ll fatal           # Only log fatal errors (not info/debug messages)
-r simple            # Use simple reporter (minimal output)
--disable-dots       # Don't print progress dots
--abort              # Stop on first failure (don't run remaining tests)
```

Example:
```bash
./stellar-core test --ll fatal -r simple --disable-dots --abort "test name"
```

Note that if you ever do need information about a test when trying to diagnose
what went wrong with it, you might want to turn the log level up from fatal to
info, debug or even trace, using `--ll debug` or `--ll trace` for example.

# Protocol Versions

Many tests are protocol-specific and can behave differently across protocol
versions. Use these flags to control which protocol versions are tested:

```bash
--version <N>        # Run tests for a specific protocol version
--all-versions       # Run tests for all supported protocol versions
```

For focused testing during development, test with the current protocol version,
which is the default. The full test suite should eventually be run with
`--all-versions`.

# Deterministic Random Number Generator

Tests use a deterministic PRNG. By default, the seed varies, but you can set
a specific seed for reproducibility:

```bash
--rng-seed <N>       # Use a specific RNG seed for reproducibility
```

This is useful for reproducing failures or for baseline checks that require
consistent output.

# Test Levels

Tests are run in order of increasing cost. Stop at the first failure.

## Level 1: Smoke Tests

Run 2-3 specific tests that are most likely to catch breakage in the changed
code. These should complete in seconds.

To identify smoke tests:
1. Find tests in the same file/module as the changed code
2. Pick tests that directly exercise the modified functions
3. Prefer fast tests over slow ones

```bash
# Run a specific test by name (use quotes for exact match)
./stellar-core test --ll fatal -r simple --abort "exact test name"
```

## Level 2: Focused Unit Tests

Run all tests in the test file(s) related to the change. This typically takes
a few minutes.

```bash
# Run tests matching a tag pattern
./stellar-core test --ll fatal -r simple --abort "[ModuleName*]"

# Run tests from a specific area
./stellar-core test --ll fatal -r simple --abort "[ledgertxn]"

# Combine tags (AND logic - must match all)
./stellar-core test --ll fatal -r simple --abort "[tx][soroban]"
```

### Example Test Names by Area

**Ledger/Transaction tests:**
- `"[ledgertxn]"` - LedgerTxn operations
- `"[tx][payment]"` - Payment transaction tests
- `"[tx][createaccount]"` - CreateAccount tests
- `"[tx][offers]"` - Offer/DEX tests
- `"[tx][soroban]"` - Soroban (smart contract) transaction tests

**Bucket/BucketList tests:**
- `"[bucket]"` - General bucket tests
- `"[bucketlist]"` - BucketList specific tests
- `"[bucketmergemap]"` - Bucket merge map tests

**Herder tests:**
- `"[herder]"` - General herder tests
- `"[txset]"` - Transaction set tests
- `"[transactionqueue]"` - Transaction queue tests
- `"[quorumintersection]"` - Quorum intersection tests
- `"[upgrades]"` - Protocol upgrade tests

**Overlay/Network tests:**
- `"[overlay]"` - Overlay network tests
- `"[flood]"` - Transaction flooding tests
- `"[PeerManager]"` - Peer management tests

**Crypto/Utility tests:**
- `"[crypto]"` - Cryptography tests
- `"[decoder]"` - Base32/64 encoding tests
- `"[timer]"` - VirtualClock timer tests
- `"[cache]"` - Cache implementation tests

**Soroban-specific tests:**
- `"[soroban]"` - All Soroban tests
- `"[soroban][archival]"` - State archival tests
- `"[soroban][upgrades]"` - Soroban upgrade tests

## Level 3: Full Unit Test Suite

Run the complete unit test suite. This may take 10-30 minutes.

### Basic Execution

```bash
make check
```

Or directly with quiet output:

```bash
./stellar-core test --ll fatal -r simple --disable-dots --abort
```

### Parallel Execution (faster)

For faster execution, use parallel partitions via `make check`:

```bash
# Run with partitions equal to CPU cores
NUM_PARTITIONS=$(nproc) make check
```

### Full Protocol Coverage

The full test suite should be run with all protocol versions:

```bash
ALL_VERSIONS=1 NUM_PARTITIONS=$(nproc) make check
```

### SQLite-Only Testing (No Postgres)

To test with SQLite only (faster, no Postgres dependency):

```bash
./configure --disable-postgres --enable-ccache --enable-sdfprefs
make clean && make -j $(nproc)
NUM_PARTITIONS=$(nproc) make check
```

## Level 3b: Transaction Metadata Baseline Check

This validates that transaction test execution produces the same metadata hashes
as fixed baselines stored in the repository. This catches unintended changes to
transaction semantics.

**Important**: Always use `--rng-seed 12345` for baseline checks to ensure
deterministic results.

```bash
# Check transaction tests against current protocol baseline
./stellar-core test "[tx]" --all-versions --rng-seed 12345 --ll fatal \
    --abort -r simple --check-test-tx-meta test-tx-meta-baseline-current
```

For next-protocol testing (when preparing protocol upgrades):

```bash
./stellar-core test "[tx]" --all-versions --rng-seed 12345 --ll fatal \
    --abort -r simple --check-test-tx-meta test-tx-meta-baseline-next
```

If baselines need updating after intentional changes, the test will fail and
indicate which baselines differ.

## Level 4: Sanitizer Tests

**When to run**: Only needed for changes touching memory management, pointers,
concurrency, or threading code. Skip for simple logic changes, config changes,
or test-only changes.

Run tests with sanitizers enabled to catch memory errors and undefined behavior.
This requires reconfiguring and rebuilding.

### Address Sanitizer (ASan)

Catches memory errors: buffer overflows, use-after-free, memory leaks.

```bash
./configure --enable-asan --enable-ccache --enable-sdfprefs
make clean && make -j $(nproc)
./stellar-core test --ll fatal -r simple --disable-dots --abort
```

### Thread Sanitizer (TSan)

Catches data races and threading issues.

```bash
./configure --enable-threadsanitizer --enable-ccache --enable-sdfprefs
make clean && make -j $(nproc)
./stellar-core test --ll fatal -r simple --disable-dots --abort
```

### Undefined Behavior Sanitizer (UBSan)

Catches undefined behavior like integer overflow, null pointer dereference.

```bash
./configure --enable-undefinedcheck --enable-ccache --enable-sdfprefs
make clean && make -j $(nproc)
./stellar-core test --ll fatal -r simple --disable-dots --abort
```

## Level 5: Extra Checks Build

**When to run**: Only for changes to core data structures or when Level 4
sanitizers found something suspicious. Usually overkill.

Run with C++ standard library debugging enabled. Slower but catches more issues.

```bash
./configure --enable-extrachecks --enable-ccache --enable-sdfprefs
make clean && make -j $(nproc)
./stellar-core test --ll fatal -r simple --disable-dots --abort
```

# Build Verification

Before running tests at Levels 4-6, also verify the build succeeds with
`--disable-tests` (the production configuration):

```bash
./configure --disable-tests --enable-ccache --enable-sdfprefs
make clean && make -j $(nproc)
```

This doesn't run tests but ensures the production build works.

# Interpreting Failures

When a test fails:

1. **Identify the failing test**: Note the exact test name and file
2. **Capture the failure output**: Save the error message and stack trace
3. **Determine if it's a real failure**: Check if the test is flaky or if this
   is a genuine regression
4. **Locate the relevant code**: Find where in the changed code the failure
   originates

## Common Failure Patterns

- **Assertion failure**: A test assertion didn't hold; check the condition
- **Crash/segfault**: Memory error; run with ASan for more details
- **Timeout**: Test took too long; may indicate infinite loop or deadlock
- **Sanitizer error**: Memory or threading bug; the sanitizer output shows where

# Output Format

Report the results:

```
## Test Results: PASS

All test levels completed successfully:
- Level 1 (Smoke): 3 tests, 2.1s
- Level 2 (Focused): 47 tests, 1m 12s
- Level 3 (Full Suite): 1,234 tests, 18m 45s
- Level 3b (TX Meta Baseline): OK

Build verification:
- --disable-tests: OK
```

Or on failure:

```
## Test Results: FAIL

Failed at Level 2 (Focused Unit Tests)

**Failing test:** `LedgerManagerTests.processTransactionRejectsEmpty`
**File:** src/ledger/LedgerManagerTests.cpp:142
**Error:**
    REQUIRE( result == TRANSACTION_REJECTED )
    with expansion:
    TRANSACTION_SUCCESS == TRANSACTION_REJECTED

**Analysis:** The test expects empty transactions to be rejected, but the
new code path is allowing them through. See LedgerManager.cpp:98 where the
empty check appears to be missing.

Levels completed before failure:
- Level 1 (Smoke): 3 tests, 2.1s ✓
```

# Choosing the Right Test Level

**For most changes** (logic fixes, new features, refactors):
- Run through Level 3 (full suite) with `--all-versions`
- Run Level 3b (tx-meta baseline) for transaction-related changes
- Skip Levels 4-5 unless the change touches memory/threading

**For memory-sensitive changes** (pointers, allocations, C++ containers):
- Run through Level 4 (at least ASan)

**For concurrency changes** (threading, async, locks):
- Run through Level 4 (especially TSan)

**For test-only changes** or documentation:
- Level 1-2 is usually sufficient

# ALWAYS

- ALWAYS run tests in order of increasing cost
- ALWAYS stop at the first failure (use `--abort` flag)
- ALWAYS use `--ll fatal -r simple --disable-dots` for quiet output
- ALWAYS capture and report failure details
- ALWAYS run full suite with `--all-versions` before considering complete
- ALWAYS use `--rng-seed 12345` for tx-meta baseline checks
- ALWAYS report timing for each level
- ALWAYS identify the specific test and location of failures

# NEVER

- NEVER skip smoke tests and go straight to full suite
- NEVER continue to later levels after a failure
- NEVER report "tests failed" without specifics
- NEVER assume a test failure is flaky without evidence
- NEVER run verbose output that floods the context
- NEVER run tests without having built first
- NEVER run sanitizers (Level 4-5) for trivial changes (it's overkill)

# Completion

Report to the invoking agent:

1. Overall result: PASS or FAIL
2. For PASS: Summary of all levels completed with timing
3. For FAIL: Detailed failure report with analysis
4. Any observations (slow tests, warnings, etc.)
