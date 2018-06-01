[![Build Status](https://travis-ci.org/stellar/stellar-core.svg?branch=auto)](https://travis-ci.org/stellar/stellar-core)


# stellar-core

Stellar-core is a replicated state machine that maintains a local copy of a cryptographic ledger and processes transactions against it, in consensus with a set of peers.
It implements the [Stellar Consensus Protocol](https://github.com/stellar/stellar-core/blob/master/src/scp/readme.md), a _federated_ consensus protocol.
It is written in C++11 and runs on Linux, OSX and Windows.
Learn more by reading the [overview document](https://github.com/stellar/stellar-core/blob/master/docs/readme.md).

# Documentation

Documentation of the code's layout and abstractions, as well as for the
functionality available, can be found in
[`./docs`](https://github.com/stellar/stellar-core/tree/master/docs).

# Installation

See [Installation](./INSTALL.md)

# Contributing

See [Contributing](./CONTRIBUTING.md)

# Running tests

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

# Running tests against postgreSQL

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

# Running tests in parallel

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

# Running stress tests
We adopt the convention of tagging a stress-test for subsystem foo as [foo-stress][stress][hide].

Then, running
* `stellar-core --test [stress]` will run all the stress tests,
* `stellar-core --test [foo-stress]` will run the stress tests for subsystem foo alone, and
* neither `stellar-core --test` nor `stellar-core --test [foo]` will run stress tests.


