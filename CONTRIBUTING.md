# How to contribute

👍🎉 First off, thanks for taking the time to contribute! 🎉👍

Check out the [Stellar Contribution
Guide](https://github.com/stellar/.github/blob/master/CONTRIBUTING.md) for details on contributing
to stellar-core and Stellar's other repositories, especially with regard to our code of conduct and
contributor license agreement.

## Submitting Changes

Fork the `stellar/stellar-core` repo on github and submit a pull request from a branch in your fork. 
Key things to keep in mind:
* Keep your branch up-to-date by rebasing on `stellar/stellar-core` `master`
* Ensure your branch focuses on a single issue at a time
* Before merging, squash your commits and ensure you have a descriptive commit message

## Stellar Core Contribution Specifics

### General
* Try to separate logically distinct changes into separate commits and thematically distinct
  commits into separate pull requests.
* Please ensure that all tests pass before submitting changes. The local testsuite can be run as
  `make check` or `src/stellar-core test`, see [Running Tests](#running-tests).

### Keeping our commit history clean

We're striving to keep master's history with minimal merge bubbles. To achieve this, we're asking
PRs to be submitted rebased on top of master.

To keep your local repository in a "rebased" state, simply run:
* `git config branch.autosetuprebase always` _changes the default for all future branches_
* `git config branch.master.rebase true` _changes the setting for branch master_

Note: you may still have to run manual "rebase" commands on your branches, to rebase on top of
master as you pull changes from upstream.

### Testing

Please ensure that all tests pass before submitting changes. The local testsuite can be run as
`make check` or `src/stellar-core --test`, see [Running Tests](#running-tests).

### Code Style

Code formatting wise, we have a `.clang-format` config file that you should use on modified files.

- Names of structs and classes are camel case with the first letter capital, eg. `class MyClassName`
- Names of functions and local variables are camel case with the first letter lowercase, eg. `void
  myFunctionName()` or `int myLocalInt`
- Names of member variables are camel case with the first letter "m", eg. `int mMyMemberInt`
- Names of compile-time constants are macro case, eg. `int const MY_CONST_INT`
- Braces appear on their own line
- Braces are not omitted for a control statement with only a single statement in its scope
- const appears to the right of the type, eg. `int const` or `int const&`

### Performance Enhancements

When submitting changes that may impact performance, you need to also provide some evidence of the
improvement (which also implies no regression). See the [performance
evaluation](../performance-eval/performance-eval.md) document for more details.

### Special configure flags for improved diagnostics

When building with `configure`, a few options are available to enable better diagnostics when
running tests.

It is strongly recommended to enable C++ runtime checks and the address sanitizer during
development by passing the following flags to configure (in addition to other flags that you may
need):

    ./configure --enable-extrachecks --enable-asan

#### enable-extrachecks
This enables additional debug checks such as passed the end iterators.

More information can be found:
* [libstdc++ debug mode](https://gcc.gnu.org/onlinedocs/libstdc++/manual/debug_mode.html)
* [libc++ debug mode](https://libcxx.llvm.org/docs/DesignDocs/DebugMode.html#using-debug-mode)
  * Note that when using the default libc++, we set `_LIBCPP_DEBUG=0` to avoid compatibility issues with the default shared runtimes.
  * To enable full debug mode `_LIBCPP_DEBUG=1`, you need to build a custom libc++ with the same flags, including `_LIBCPP_DEBUG=1` (see below on how to do this)

#### enable-codecoverage
This enables source based code coverage.

More information can be found [here](https://clang.llvm.org/docs/SourceBasedCodeCoverage.html)

Steps (tested on Mac OS):

1. Run `./configure --enable-codecoverage` (include `--enable-next-protocol-version-unsafe-for-production` if testing a future protocol version)
2. Compile stellar core
3. Run the desired test, but with the LLVM_PROFILE_FILE environment variable set (ex. `LLVM_PROFILE_FILE="cov.profraw" src/stellar-core test '[liquiditypool]'`). This should generate a cov.profraw file in the current directory.
4. Run `xcrun llvm-profdata merge -output=cov.profdata cov.profraw`
   * the llvm tools are only accessible on Mac OS through `xcrun`
5. Run `xcrun llvm-cov show ./src/stellar-core -instr-profile=cov.profdata -format="html" > cov.html`
   * The resulting html file can be around ~45MB, so it can be difficult to navigate with a
   browser. You can use the `-ignore-filename-regex` option to remove files/directories that you aren't
   interested in (ex. The following option excludes every directory specified `-ignore-filename-regex='.*lib[/\\].*|.*bucket[/\\].*|.*catchup[/\\].*|.*crypto[/\\].*|.*database[/\\].*|.*herder[/\\].*|.*history[/\\].*|.*historywork[/\\].*|.*overlay[/\\].*|.*xdr[/\\].*|.*sodium[/\\].*|.*work[/\\].*|.*test[/\\].*|.*scp[/\\].*|.*main[/\\].*|.*simulation[/\\].*|.*invariant[/\\].*|.*util[/\\].*'`)


### Special configure flags for unreleased protocol versions

When building with `configure`, the flag below must be used to enable unreleased protocol
versions. If this flag is not provided code and tests relating to the next protocol version will
not execute.

    ./configure --enable-next-protocol-version-unsafe-for-production

## Sanitizers

Sanitizers are mutually exclusive.

### enable-asan
Build with asan (address-sanitizer) instrumentation, which detects invalid address utilization.

See https://clang.llvm.org/docs/AddressSanitizer.html for more information.

### enable-undefinedcheck
build with undefinedcheck (undefined-behavior-sanitizer) instrumentation.

See https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html for more information.

### enable-memcheck
build with memcheck (memory-sanitizer) instrumentation.
See https://clang.llvm.org/docs/MemorySanitizer.html for more information.

`memcheck` only works with clang and `libc++`.

For memcheck to work, you will need to compile your own version of `libc++` (see below), and pass
the path to your libraries to the configure script using the `LIBCXX_PATH` variable, something
like:

    ./configure --disable-postgres --enable-memcheck LIBCXX_PATH=/home/user/src/llvm/libcxx_msan/lib

If you do not have an instrumented version of `libpq` (postgres client library), you may get false
positives as well (disabling postgres is a good workaround).

What the configure script does under the cover is use the custom version of your library at link
time, but still uses the system headers - so make sure that the two don't conflict!

### enable-threadsanitizer
Build with thread sanitizer (TSan) instrumentation, which detects data races.

See https://clang.llvm.org/docs/ThreadSanitizer.html for more information.

#### Building a custom `libc++`

The steps for building an instrumented version of libc++ can be found on the [memory sanitizer
how-to](https://github.com/google/sanitizers/wiki/MemorySanitizerLibcxxHowTo#instrumented-libc).

When done, note the path to `libc++.so` and `libc++abi.so` (that will be passed with `LIBCXX_PATH`
to the configure script).

# Running tests

There are two ways to run tests:

- `src/stellar-core test`
- `make check`

Always build before running tests, unless using `make check` which will build for you. See [INSTALL.md](./INSTALL.md) for instructions for how to build.

## Running tests basics with `src/stellar-core test`
run tests with:
  `src/stellar-core test`

run one test with:
  `src/stellar-core test  testName`

run one test category with:
  `src/stellar-core test '[categoryName]'`

Categories (or tags) can be combined: AND-ed (by juxtaposition) or OR-ed (by comma-listing).

Tests tagged as `[.]` or `[hide]` are not part of the default test.

Tests tagged as `[acceptance]` are not part of `make check` test runs.

supported test options can be seen with
  `src/stellar-core test --help`

display tests timing information:
  `src/stellar-core test -d yes '[categoryName]'`

xml test output (includes nested section information):
  `src/stellar-core test -r xml '[categoryName]'`

Tests may also be run with `make check`, see [Running tests in parallel](#running tests-in-parallel-with-make-check).

## Running tests against postgreSQL

There are two options.  The easiest is to have the test suite just
create a temporary postgreSQL database cluster in /tmp and delete it
after the test.  That will happen by default if you run `make check`.

You can also use an existing database cluster so long as it has
databases named `test0`, `test1`, ..., `test9`, and `test`.  To set
this up, make sure your `PGHOST` and `PGUSER` environment variables
are appropriately set, then run the following from bash:

    for i in $(seq 0 9) ''; do
        psql -c "create database test$i;"
    done

You will need to set the `TEMP_POSTGRES` environment variable to 0
in order to use an existing database cluster.

## Running tests in parallel with `make check`

The `make check` command runs tests and supports parallelization. This functionality is enabled with the following environment variables:
* `ALL_VERSIONS`: If 0, runs the latest protocol version, if 1 runs all protocol version tests.
* `TEST_SPEC`: Used to run just a subset of the tests (default: "~[.]")
* `NUM_PARTITIONS`: Partitions the test suite (after applying `TEST_SPEC`) into
`$NUM_PARTITIONS` disjoint sets (default: 1)
* `BATCHSIZE`: The number of tests to be batched together to reduce setup overhead. (default: 5)
* `RUN_PARTITIONS`: Run only a subset of the partitions, indexed from 0
(default: "$(seq 0 $((NUM_PARTITIONS-1)))")
* `TEMP_POSTGRES`: Automatically generates temporary database clusters instead
of using an existing cluster (default: 1)
* `RND_SEED`: Can be set to a specific value to affect the random test ordering. (default: 1)

For example,
`env TEST_SPEC="[history]" NUM_PARTITIONS=4 RUN_PARTITIONS="0 1 3" make check`
will partition the history tests into 4 parts then run parts 0, 1, and 3.

## Running stress tests

There are a few special stress tests included in the test suite. Those are *subsystem level* tests,
not to be confused with more advanced tests that would be done as part of [performance
evaluation](../performance-eval/performance-eval.md).

We adopt the convention of tagging a stress-test for subsystem foo as [foo-stress][stress][hide].

Then, running:

* `stellar-core test [stress]` will run all the stress tests,
* `stellar-core test [foo-stress]` will run the stress tests for subsystem foo alone, and
* neither `stellar-core test` nor `stellar-core test [foo]` will run stress tests.

## Running and updating TxMeta checks

The `stellar-core test` unit tests can be run in two special modes that hash the
TxMeta of each transaction executed. These two modes can increase confidence
that a change to stellar-core does not alter the semantics of any transactions.
The two modes are:

  * `--record-test-tx-meta <dirname>` which records TxMeta hashes into `<dirname>`
  * `--check-test-tx-meta <dirname>` which checks TxMeta hashes against `<dirname>`

Continuous integration tests automatically run the `--check-test-tx-meta` mode
against a pair of captured baseline directories stored in the repository, called
`test-tx-meta-baseline-current` (for the current protocol) and
`text-tx-meta-baseline-next` (for the next protocol). If you make _intentional_
changes to the semantics of any transactions, or add any new transactions that
need to have their hashes recorded, you can re-record the baseline using a
command like:

    stellar-core test [tx] --all-versions --rng-seed 12345 --record-test-tx-meta test-tx-meta-baseline-current

for a build with only the current protocol enabled, and:

    stellar-core test [tx] --all-versions --rng-seed 12345 --record-test-tx-meta test-tx-meta-baseline-next

for a build configured with `--enable-next-protocol-version-unsafe-for-production`.

These commands will rewrite the baseline files, which are human-readable JSON
files. You should then inspect to see that only the transactions you expected to
see change did so. If so, commit the changes as a new set of baselines for
future tests.
