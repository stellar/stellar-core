[![Build Status](https://travis-ci.org/stellar/stellar-core.svg?branch=auto)]

# Note this code is pre-beta. 
It is definitely not ready yet for production.
 
#stellar-core

Stellar-core is a C++ implementation of the [Stellar Consensus Protocol](https://github.com/stellar/stellar-core/blob/master/src/scp/readme.md) that maintains a local copy of the ledger and syncs with other instances of stellar-core on the network. Learn more by reading the [overview document](https://github.com/stellar/stellar-core/blob/master/docs/readme.md).

# Documentation

Documentation of the code's layout and abstractions, as well as for the
functionality available, can be found in
[`./docs`](https://github.com/stellar/stellar-core/tree/master/docs).

# Contributing

We're striving to keep master's history with minimal merge bubbles. To achieve
this, we're asking PRs to be submitted rebased on top of master.

To keep your local repository in a "rebased" state, simply run:

`git config --global branch.autosetuprebase always` *changes the default for all future branches*

`git config --global branch.master.rebase true` *changes the setting for branch master*

note: you may still have to run manual "rebase" commands on your branches to rebase on top of master as you pull changes from upstream.

Code formatting wise, we have a `.clang-format` config file that you should use on modified files.

Please sign the [Contributor License Agreement](http://goo.gl/forms/f2nhGi537n).

# Running tests

run tests with:
  `bin/stellar-core --test`

run one test with:
  `bin/stellar-core --test  testName`

run one test category with:
  `bin/stellar-core --test '[categoryName]'`

Categories (or tags) can be combined: AND-ed (by juxtaposition) or OR-ed (by comma-listing).

Tests tagged as [.] or [hide] are not part of the default test test.

# Running tests against postgreSQL
First you'll need to create the postgres role "test". If you've just installed postgresSQL with the default init, access the psql client by running `psql postgres`.

Now create the 'test' role: `CREATE ROLE test SUPERUSER`

Now, create a few test databases:
`CREATE DATABASE test WITH owner=test`

`CREATE DATABASE test0 WITH owner=test`

`CREATE DATABASE test1 WITH owner=test`

`CREATE DATABASE test2 WITH owner=test`

# Running stress tests
We adopt the convention of tagging a stress-test for subsystem foo as [foo-stress][stress][hide].

Then, running
* `stellar-core --test [stress]` will run all the stress tests,
* `stellar-core --test [foo-stress]` will run the stress tests for subsystem foo alone, and
* neither `stellar-core --test` nor `stellar-core --test [foo]` will run stress tests.

