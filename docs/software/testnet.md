---
title: Testnet
---

Review the [admin guide](./admin.md) for more detailed information.

## Starting a test network with 1 node

First, make sure you have copied the example config to your current working directory.
From the TLD of the repo, run
`cp docs/stellar_core_standalone.cfg ./bin/stellar-core.cfg`

By default stellar-core waits to hear from the network for a ledger close before
it starts emitting its own SCP messages. This works fine in the common case but
when you want to start your own network you need to start SCP manually.
this is done by:

```sh
$ stellar-core force-scp
```

That will set state in the DB and then exit. The next time you start
stellar-core SCP will start immediately rather than waiting.


## Adding multiple nodes

For each node on your new network:
* generate a keypair and set `NODE_SEED`
* set `RUN_STANDALONE=false` and `NODE_IS_VALIDATOR=true`
* set the `QUORUM_SET` and `KNOWN_PEERS` to refer to one another
* decide on a history archive and add a HISTORY config entry for it
* Set the `DATABASE` config variables on each node to your choice of database

Optionally: Create databases for each to use--e.g., by using PostgreSQL's `createdb` command.

Run:

1. `$ stellar-core new-hist <historyarchive>`
  - to initialize every history archive you are putting to (be sure to not push to the same archive from different nodes).
2. `$ stellar-core new-db`
  - to initialize the database on each node. 
3. `$ stellar-core force-scp`
  - to set a flag to force each node to start SCP immediatly rather than wait to hear from the network. 
4. `$ stellar-core run`
  - on each node to start it.

## Bringing a test network back up
If you need to restart the network after bringing it down.

Stop all nodes, and do the following on nodes that all have the same last ledger (NB: this set must form a quorum in order to reach consensus):

```sh
$ stellar-core force-scp
$ stellar-core run
```

This will start from the last saved state of each server. After these servers sync you can start the other nodes in the cluster normally and they will catch up to the network.

