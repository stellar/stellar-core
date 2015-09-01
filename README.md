[![Build Status](https://travis-ci.org/stellar/stellar-core.svg?branch=auto)](https://travis-ci.org/stellar/stellar-core)

# Note this code is pre-beta. 
It is definitely not ready yet for production.
 
# stellar-core

Stellar-core is a C++ implementation of the [Stellar Consensus Protocol](https://github.com/stellar/stellar-core/blob/master/src/scp/readme.md) that maintains a local copy of the ledger and syncs with other instances of stellar-core on the network. Learn more by reading the [overview document](https://github.com/stellar/stellar-core/blob/master/docs/readme.md).

# Documentation

Documentation of the code's layout and abstractions, as well as for the
functionality available, can be found in
[`./docs`](https://github.com/stellar/stellar-core/tree/master/docs).

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

# Running tests against postgreSQL

There are two options.  The easiest is to have the test suite just
create a temporary postgreSQL database cluster in /tmp and delete it
after the test.  That will happen by default if you run `make check`.

You can also create a temporary database cluster manually, by running
`./src/test/selftest-pg bash` to get a shell, then running tests
manually.  The advantage of this is that you can examine the database
log in `$PGDATA/pg_log/` after running tests, as well as manually
inspect the database with `psql`.

Finally, you can use an existing database cluster so long as it has
databases named `test0`, `test1`, ..., `test8`, and `test`.  Do set
this up, make sure your `PGHOST` and `PGUSER` environment variables
are appropriately set, then run the following from bash:

    for i in $(seq 0 8) ''; do
        psql -c "create database test$i;"
    done

# Running stress tests
We adopt the convention of tagging a stress-test for subsystem foo as [foo-stress][stress][hide].

Then, running
* `stellar-core --test [stress]` will run all the stress tests,
* `stellar-core --test [foo-stress]` will run the stress tests for subsystem foo alone, and
* neither `stellar-core --test` nor `stellar-core --test [foo]` will run stress tests.
