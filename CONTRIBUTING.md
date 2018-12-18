# How to contribute

We're striving to keep master's history with minimal merge bubbles. To achieve this, we're asking PRs to be submitted rebased on top of master.

To keep your local repository in a "rebased" state, simply run:

`git config --global branch.autosetuprebase always` _changes the default for all future branches_

`git config --global branch.master.rebase true` _changes the setting for branch master_

Note: you may still have to run manual "rebase" commands on your branches, to rebase on top of master as you pull changes from upstream.

## Finding things to work on
The first place to start is always looking over the current github issues for the project you are interested in contributing to. Issues marked with `help wanted` are usually pretty self contained and a good place to get started.
stellar.org also uses these same github issues to keep track of what we are working on. If you see any issues that are assigned to a particular person that means someone is currently working on that issue. 

Of course feel free to make your own issues if you think something needs to added or fixed.

# Basic quality checks

Please ensure that all tests pass before submitting changes. The local testsuite can be run as `make check` or `src/stellar-core --test`,
see [README](./README.md) for details on running tests.

Code formatting wise, we have a `.clang-format` config file that you should use on modified files.

Try to separate logically distinct changes into separate commits and thematically distinct commits into separate pull requests.

# Performance impacting changes

When submitting changes that may impact performance, you need to also provide some evidence of the improvement (which also implies no regression). See the [performance evaluation](../performance-eval/performance-eval.md) document for more details.

# Submitting Changes

Please [sign the Contributor License Agreement](https://docs.google.com/forms/d/1g7EF6PERciwn7zfmfke5Sir2n10yddGGSXyZsq98tVY/viewform).

All content, comments, and pull requests must follow the [Stellar Community Guidelines](https://www.stellar.org/community-guidelines/). 

Submit a pull request rebased on top of master

 * Include a descriptive commit message.
 * Changes contributed via pull request should focus on a single issue at a time.
 
At this point you're waiting on us. We like to at least comment on pull requests within one week (and, typically, three business days). We may suggest some changes or improvements or alternatives.

# Special configure flags for improved diagnostics

When building with `configure`, a few options are available to enable better diagnostics when running tests.

It is strongly recommended to enable c++ runtime checks and address sanitizer during development by passing the following flags to configure (in addition to other flags that you may need):

    ./configure --enable-extrachecks --enable-asan

## enable-extrachecks
This enables additional debug checks such as passed the end iterators.

More information can be found:
* [libstdc++ debug mode](https://gcc.gnu.org/onlinedocs/libstdc++/manual/debug_mode.html)
* [libc++ debug mode](https://libcxx.llvm.org/docs/DesignDocs/DebugMode.html#using-debug-mode)


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

For memcheck to work, you will need to compile your own version of `libc++` (see below), and pass the path to your libraries to the configure script using the `LIBCXX_PATH` variable, something like:

    ./configure --disable-postgres --enable-memcheck LIBCXX_PATH=/home/user/src/llvm/libcxx_msan/lib

If you do not have an instrumented version of `libpq` (postgres client library), you may get false positives as well (disabling postgres is a good workaround).

What the configure script does under the cover is use the custom version of your library at link time, but still uses the system headers - so make sure that the two don't conflict!

#### Building a custom `libc++`

The steps for building an instrumented version of libc++ can be found on the [memory sanitizer how-to](https://github.com/google/sanitizers/wiki/MemorySanitizerLibcxxHowTo#instrumented-libc).

When done, note the path to `libc++.so` and `libc++abi.so` (that will be passed with `LIBCXX_PATH` to the configure script).

# Running tests

## Running tests basics
run tests with:
  `src/stellar-core --test`

run one test with:
  `src/stellar-core --test  testName`

run one test category with:
  `src/stellar-core --test '[categoryName]'`

Categories (or tags) can be combined: AND-ed (by juxtaposition) or OR-ed (by comma-listing).

Tests tagged as [.] or [hide] are not part of the default test test.

supported test options can be seen with
  `src/stellar-core --test --help`

display tests timing information:
  `src/stellar-core --test -d yes '[categoryName]'`

xml test output (includes nested section information):
  `src/stellar-core --test -r xml '[categoryName]'`

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

## Running tests in parallel

The `make check` command also supports parallelization. This functionality is
enabled with the following environment variables:
* `TEST_SPEC`: Used to run just a subset of the tests (default: "~[.]")
* `NUM_PARTITIONS`: Partitions the test suite (after applying `TEST_SPEC`) into
`$NUM_PARTITIONS` disjoint sets (default: 1)
* `RUN_PARTITIONS`: Run only a subset of the partitions, indexed from 0
(default: "$(seq 0 $((NUM_PARTITIONS-1)))")
* `TEMP_POSTGRES`: Automatically generates temporary database clusters instead
of using an existing cluster (default: 1)

For example,
`env TEST_SPEC="[history]" NUM_PARTITIONS=4 RUN_PARTITIONS="0 1 3" make check`
will partition the history tests into 4 parts then run parts 0, 1, and 3.

## Running stress tests

There are a few special stress tests included in the test suite. Those are *subsystem level* tests, not to be confused with more advanced tests that would be done as part of [performance evaluation](../performance-eval/performance-eval.md).

We adopt the convention of tagging a stress-test for subsystem foo as [foo-stress][stress][hide].

Then, running
* `stellar-core --test [stress]` will run all the stress tests,
* `stellar-core --test [foo-stress]` will run the stress tests for subsystem foo alone, and
* neither `stellar-core --test` nor `stellar-core --test [foo]` will run stress tests.

