---
title: Administration
---

Stellar Core is responsible for communicating directly with and/or maintaining 
the Stellar peer-to-peer network.

## Why run a node?

Run stellar-core if you want to:
* Obtain the most up-to-date and reliable data from the Stellar network
* Generate extended meta data on activity within the Stellar network (change tracking, etc)
* Submit transactions and their confirmations without depending on a third party
* Extended control on which parties to trust in the Stellar network
* Participate in validating the Stellar network


## Building
See [readme](https://github.com/stellar/stellar-core/blob/master/README.md) for build instructions.

## Configuring
All configuration for stellar-core is done with a TOML file. By default 
stellar-core loads `./stellar-core.cfg`, but you can specify a different file 
to load on the command line: `> stellar-core --conf betterfile.cfg` 

The [example config](https://github.com/stellar/stellar-core/blob/master/docs/stellar-core_example.cfg) describes all the possible 
configuration options.  

## Running
Stellar-core can be run directly from the command line, or through a supervision 
system such as `init`, `upstart`, or `systemd`.

Stellar-core sends logs to standard output and `stellar-core.log` by default, 
configurable as `LOG_FILE_PATH`.
 Log messages are classified by progressive _priority levels_:
  `TRACE`, `DEBUG`, `INFO`, `WARNING`, `ERROR` and `FATAL`.
   The logging system only emits those messages at or above its configured logging level. 

The log level can be controlled by configuration, by the `-ll` command-line flag 
or adjusted dynamically by administrative (HTTP) commands.
 Run `stellar-core -c "ll?level=debug"` against a running system.
Log levels can also be adjusted on a partition-by-partition basis through the 
administrative interface.
 For example the history system can be set to DEBUG-level logging by running
`stellar-core -c "ll?level=debug&partition=history"` against a running system.
 The default log level is `INFO`, which is moderately verbose and should emit 
 progress messages every few seconds under normal operation.

Stellar-core can be gracefully exited at any time by delivering `SIGINT` or
 pressing `CTRL-C`. It can be safely, forcibly terminated with `SIGTERM` or
  `SIGKILL`. The latter may leave a stale lock file in the `BUCKET_DIR_PATH`,
   and you may need to remove the file before it will restart. 
   Otherwise, all components are designed to recover from abrupt termination.

Stellar-core can also be packaged in a container system such as Docker, so long 
as `BUCKET_DIR_PATH`, `TMP_DIR_PATH`, and the database are stored on persistent 
volumes. For an example, see [docker-stellar-core](https://github.com/stellar/docker-stellar-core).

## Administrative commands
While running, interaction with stellar-core is done via an administrative 
HTTP endpoint. Commands can be submitted using command-line HTTP tools such 
as `curl`, or by `stellar-core -c <command>`. The endpoint is not intended to 
be exposed to the public internet. It's typically accessed by administrators, 
or by a mid-tier application to submit transactions to the Stellar network. 
See [commands](./commands.md) for a description of the available commands.

## Hardware requirements
The hardware requirements scale with the amount of activity in the network. 
Currently stellar-core requires very modest hardware. It would be fine to run 
on an AWS micro instance, for example.

# Configuration Choices

## Validating
Nodes are considered **validating** if they take part in SCP and sign messages 
pledging that the network agreed to a particular transaction set. It isn't 
necessary to be a validator. Only set your node to validate if other nodes 
care about your validation. 

If you want to validate, you must generate a public/private key for your node.
 Nodes shouldn't share keys. You should carefully secure your private key. 
If it is compromised, someone can send false messages to the network and those 
messages will look like they came from you. 

Generate a key pair like this:
`stellar-core --genseed`
Place the seed in your config:
`NODE_SEED="SBI3CZU7XZEWVXU7OZLW5MMUQAP334JFOPXSLTPOH43IRTEQ2QYXU5RG"`
and set the following value in your config:
`NODE_IS_VALIDATOR=true`
Advertise the public key so people can add it to their `QUORUM_SET` in their config.
If you don't include a `NODE_SEED` or set `NODE_IS_VALIDATOR=true`, you will still
watch SCP and see all the data in the network but will not send validation messages.

## Database
Stellar-core stores the state of the ledger in a SQL database. This DB should 
either be a SQLite database or, for larger production instances, a separate 
PostgreSQL server. For how to specify the database, 
see [example config](https://github.com/stellar/stellar-core/blob/master/docs/stellar-core_example.cfg).

When running stellar-core for the first time, you must initialize the database:
`> stellar-core --newdb`
This command will initialize the database and then exit. You can also use this 
command if your DB gets corrupted and you want to restart it from scratch. 

## Buckets
Stellar-core stores a duplicate copy of the ledger in the form of flat XDR files 
called "buckets." These files are placed in a directory specified in the config 
file as `BUCKET_DIR_PATH`, which defaults to `buckets`. The bucket files are used
 for hashing and transmission of ledger differences to history archives. This 
 directory must be on the same file system as the configured temporary 
 directory `TMP_DIR_PATH`. For the most part, the contents of both directories 
can be ignored--they are managed by stellar-core, but they should be stored on 
a fast local disk with sufficient space to store several times the size of the 
current ledger. 

## History archives
Stellar-core normally interacts with one or more "history archives," which are 
configurable facilities for storing and retrieving flat files containing history 
checkpoints: bucket files and history logs. History archives are usually off-site 
commodity storage services such as Amazon S3, Google Cloud Storage, 
Azure Blob Storage, or custom SCP/SFTP/HTTP servers. 

Use command templates in the config file to give the specifics of which 
services you will use and how to access them. 
The [example config](https://github.com/stellar/stellar-core/blob/master/docs/stellar-core_example.cfg) 
shows how to configure a history archive through command templates. 

While it is possible to run a stellar-core node with no configured history 
archives, it will be _severely limited_, unable to participate fully in a 
network, and likely unable to acquire synchronization at all. At the very 
least, if you are joining an existing network in a read-only capacity, you 
will still need to configure a `get` command to access that network's history 
archives.

# Recipes

## Joining an existing network

Put the network's `KNOWN_PEERS`, `QUORUM_SET`, and `HISTORY` details in a config file.
Optionally: If you're going to be a validating node, generate key pair and 
set `NODE_SEED` to your seed, and `NODE_IS_VALIDATOR=true`.
Optionally: Create an external database to use--e.g., by using 
PostgreSQL's `createdb` command.
Set the `DATABASE` config variable to your choice of database.
Run `stellar-core --newdb` to initialize the database.
Run `stellar-core`.

## Starting a new network

Generate a keypair for each node, and set `NODE_SEED` and `NODE_IS_VALIDATOR=true`
on each node of your new network.
Set the `QUORUM_SET` and `KNOWN_PEERS` of each node to refer to one another.
Decide on a history archive and add a HISTORY config entry for it on each node.
Optionally: Create databases for each to use--e.g., by using PostgreSQL's `createdb` command.
Set the `DATABASE` config variables on each node to your choice of database.
Run `stellar-core --newhist <historyarchive>` on _one_ node, to initialize the history archive.
Run `stellar-core --newdb` to initialize the database on each node.
Run `stellar-core --forcescp` to set a flag to force each node to start SCP rather than join.
Run `stellar-core` on each node.


# Notes

It can take up to 5 or 6 minutes to sync to the network when you start up. Most 
of this syncing time is simply stellar-core waiting for the next history 
checkpoint to be made in a history archive it is reading from.

## Additional documentation

This directory contains the following additional documentation:

* [testnet.md](./testnet.md) is a short tutorial demonstrating how to
  configure and run a short-lived, isolated test network.

* [architecture.md](https://github.com/stellar/stellar-core/blob/master/docs/architecture.md) 
  describes how stellar-core is structured internally, how it is intended to be 
  deployed, and the collection of servers and services needed to get the full 
  functionality and performance.
