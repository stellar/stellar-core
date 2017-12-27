---
title: Administration
---

## Purpose of this document

This document describes various aspects of running `stellar-core` for **system administrators** (but may be useful to a broader audience).

## Introduction

Stellar Core is responsible for communicating directly with and maintaining 
the Stellar peer-to-peer network. For a high-level introduction to Stellar Core, [watch this talk](https://www.youtube.com/watch?v=pt_mm8S9_WU) on the architecture and ledger basics:

[![Introduction to Stellar Core](https://i.ytimg.com/vi/pt_mm8S9_WU/hqdefault.jpg "Introduction to Stellar Core")](https://www.youtube.com/watch?v=pt_mm8S9_WU)

It will also be useful to understand how [data flows](https://www.stellar.org/developers/stellar-core/software/core-data-flow.pdf) and is stored in stellar-core.

## Why run a node?

Run stellar-core if you want to:
* Obtain the most up-to-date and reliable data from the Stellar network
* Generate extended meta data on activity within the Stellar network (change tracking, etc)
* Submit transactions and their confirmations without depending on a third party
* Extended control on which parties to trust in the Stellar network
* Participate in validating the Stellar network


## Building
See [readme](https://github.com/stellar/stellar-core/blob/master/README.md) for build instructions.
We also provide a [docker container](https://github.com/stellar/docker-stellar-core-horizon) for a potentially quicker set up than building from source.

## Package based Installation
If you are using Ubuntu 16.04 LTS we provide the latest stable releases of [stellar-core](https://github.com/stellar/stellar-core) and [stellar-horizon](https://github.com/stellar/go/tree/master/services/horizon) in Debian binary package format.

See [detailed installation instructions](https://github.com/stellar/packages#sdf---packages)

## Configuring
All configuration for stellar-core is done with a TOML file. By default 
stellar-core loads 

`./stellar-core.cfg`

, but you can specify a different file to load on the command line:

`$ stellar-core --conf betterfile.cfg` 

The [example config](https://github.com/stellar/stellar-core/blob/master/docs/stellar-core_example.cfg) describes all the possible 
configuration options.

Here is an [example test network config](https://github.com/stellar/docker-stellar-core-horizon/blob/master/testnet/core/etc/stellar-core.cfg) for connecting to the test network.

Here is an [example public network config](https://github.com/stellar/docs/blob/master/other/stellar-core-validator-example.cfg) for connecting to the public network.

The examples in this file don't specify `--conf betterfile.cfg` for brevity.

## Running
Stellar-core can be run directly from the command line, or through a supervision 
system such as `init`, `upstart`, or `systemd`.

Stellar-core sends logs to standard output and `stellar-core.log` by default, 
configurable as `LOG_FILE_PATH`.
 Log messages are classified by progressive _priority levels_:
  `TRACE`, `DEBUG`, `INFO`, `WARNING`, `ERROR` and `FATAL`.
   The logging system only emits those messages at or above its configured logging level. 

The log level can be controlled by configuration, by the `-ll` command-line flag 
or adjusted dynamically by administrative (HTTP) commands. Run:

`$ stellar-core -c "ll?level=debug"`

against a running system.
Log levels can also be adjusted on a partition-by-partition basis through the 
administrative interface.
 For example the history system can be set to DEBUG-level logging by running:

`$ stellar-core -c "ll?level=debug&partition=history"` 

against a running system.
 The default log level is `INFO`, which is moderately verbose and should emit 
 progress messages every few seconds under normal operation.

Stellar-core can be gracefully exited at any time by delivering `SIGINT` or
 pressing `CTRL-C`. It can be safely, forcibly terminated with `SIGTERM` or
  `SIGKILL`. The latter may leave a stale lock file in the `BUCKET_DIR_PATH`,
   and you may need to remove the file before it will restart. 
   Otherwise, all components are designed to recover from abrupt termination.

Stellar-core can also be packaged in a container system such as Docker, so long 
as `BUCKET_DIR_PATH`, `TMP_DIR_PATH`, and the database are stored on persistent 
volumes. For an example, see [docker-stellar-core](https://github.com/stellar/docker-stellar-core-horizon).

Note: `BUCKET_DIR_PATH` and `TMP_DIR_PATH` *must* reside on the same volume
as stellar-core needs to rename files between the two.

## Administrative commands
While running, interaction with stellar-core is done via an administrative 
HTTP endpoint. Commands can be submitted using command-line HTTP tools such 
as `curl`, or by 

`$ stellar-core -c <command>`

. The endpoint is not intended to be exposed to the public internet. It's typically accessed by administrators, 
or by a mid-tier application to submit transactions to the Stellar network. 
See [commands](./commands.md) for a description of the available commands.

## Hardware requirements
The hardware requirements scale with the amount of activity in the network. 
Currently stellar-core requires very modest hardware. It would be fine to run 
on an AWS micro instance, for example.

# Configuration Choices

## Level of participation to the network

As a node operator you can participate to the network in multiple ways.

|              | watcher | archiver | basic validator | full validator |
| ------------ | ------  | ---------| --------------  | -------------- |
| description  | non-validator | all of watcher + publish to archive | all of watcher + active participation in consensus (submit proposals for the transaction set to include in the next ledger) | basic validator + publish to archive |
| submits transactions | yes | yes | yes | yes |
| supports horizon | yes | yes | yes | yes |
| participates in consensus | no | no | yes | yes |
| helps other nodes to catch up and join the network | no | yes | no | yes |

From an operational point of view "watchers" and "basic validators" are about the
 same (they both compute an up to date version of the ledger).
"Archivers" or "Full validators" publish into an history archive which
 has additional cost.

## Validating
Nodes are considered **validating** if they take part in SCP and sign messages 
pledging that the network agreed to a particular transaction set. It isn't 
necessary to be a validator. Only set your node to validate if other nodes 
care about your validation.

If you want to validate, you must generate a public/private key for your node.
 Nodes shouldn't share keys. You should carefully *secure your private key*. 
If it is compromised, someone can send false messages to the network and those 
messages will look like they came from you.

Generate a key pair like this:

`$ stellar-core --genseed`
the output will look like
```
Secret seed: SBAAOHEU4WSWX6GBZ3VOXEGQGWRBJ72ZN3B3MFAJZWXRYGDIWHQO37SY
Public: GDMTUTQRCP6L3JQKX3OOKYIGZC6LG2O6K2BSUCI6WNGLL4XXCIB3OK2P
```

Place the seed in your config:

`NODE_SEED="SBAAOHEU4WSWX6GBZ3VOXEGQGWRBJ72ZN3B3MFAJZWXRYGDIWHQO37SY"`

and set the following value in your config:

`NODE_IS_VALIDATOR=true`

Tell other people your public key (GDMTUTQ... ) so people can add it to their `QUORUM_SET` in their config.
If you don't include a `NODE_SEED` or set `NODE_IS_VALIDATOR=true`, you will still
watch SCP and see all the data in the network but will not send validation messages.

See a [list of other validators](https://github.com/stellar/docs/blob/master/validators.md).

## Database
Stellar-core stores the state of the ledger in a SQL database. This DB should 
either be a SQLite database or, for larger production instances, a separate 
PostgreSQL server. For how to specify the database, 
see [example config](https://github.com/stellar/stellar-core/blob/master/docs/stellar-core_example.cfg).

When running stellar-core for the first time, you must initialize the database:

`$ stellar-core --newdb`

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

## Configuring to publish to an archive
Archive sections can also be configured with `put` and `mkdir` commands to
 cause the instance to publish to that archive.

The very first time you want to use your archive, you need to initialize it with:
`$ stellar-core --newhist <historyarchive>`

before starting your node.

IMPORTANT:
 * make sure that you configure both `put` and `mkdir` if `put` doesn't
 automatically create sub-folders
 * writing to the same archive from different nodes is not supported and
 will result in undefined behavior, *potentially data loss*.
 * do not run `newhist` on an existing archive unless you want to erase it

## Network configuration

The network itself has network wide settings that can be updated. This is done by validators voting for and agreeing to new values.

A node can be configured to vote for upgrades using the `upgrades` endpoint . see [`commands.md`](commands.md) for more information.

The network settings are:
 * the version of the protocol used to process transactions
 * the maximum number of transactions that can be included in a given ledger close
 * the cost (fee) associated with processing operations
 * the base reserve used to calculate the lumen balance needed to store things in the ledger

When the network time is later than the `upgradetime` specified in
the upgrade settings, the validator will vote to update the network
to the value specified in the upgrade setting.

When a validator is armed to change network values, the output of `info` will
contain information about the vote.

For a new value to be adopted, the same level of consensus between nodes needs
to be reached as for transaction sets.

### Important notes on network wide settings

Changes to network wide settings have to be orchestrated properly between
validators as well as non validating nodes:
* a change is vetted between operators (changes can be bundled)
* an effective date in the future is picked for the change to take effect (controlled by `upgradetime`)
* if applicable, communication is sent out to all network users

An improper plan may cause issues such as:
* nodes missing consensus (aka "getting stuck"), and having to use history to rejoin
* network reconfiguration taking effect at a non deterministic time (causing fees to change ahead of schedule for example)

For more information look at [`docs/versioning.md`](../versioning.md).

# Quorum

## A brief explanation
An important distinction in Stellar compared to traditional quorum based
networks is that validators that form the network do not necessarily share
 the same configuration of what a quorum is.
Quorum set represents the configuration of a specific node, where as quorum
is derived from the set of all quorum sets from the participants.

Here is a way to think about the distinction:
Imagine a game where people are put in a room and the goal is to get as many
 people to match hat color.
The rules are:
 * each person in the room can only see the people in front of them.
 * each person can pick their location in the room for the duration of the
exercise (this basically defines the player's quorum set).
 * when the game starts, light is turned on every minute for 1 second.

 SCP is the protocol (gestures, when to switch hat color, etc) that causes
 people in the room to converge to the same hat color.
The quorum here is the set of players that end up with the winning hat color.

A traditional quorum has people see everybody else (placed on a circle, at
the right distance).
Here, each player can decide to look wherever they want. Even look at only one
other player, which is a risk if this other player decides to quit the game.

## Quorum set

Configuring your QUORUM_SET properly is one of the most important thing you should be doing.

The simplest way to configure it is by using a flat QUORUM_SET, something that looks like this:
```
[QUORUM_SET]
THRESHOLD_PERCENT=70
VALIDATORS=[
"GDKXE2OZMJIPOSLNA6N6F2BVCI3O777I2OOC4BV7VOYUEHYX7RTRYA7Y sdf1",
"GCUCJTIYXSOXKBSNFGNFWW5MUQ54HKRPGJUTQFJ5RQXZXNOLNXYDHRAP sdf2",
"GC2V2EFSXN6SQTWVYA5EPJPBWWIMSD2XQNKUOHGEKB535AQE2I6IXV2Z sdf3",
...
]
```
THRESHOLD_PERCENT is simply the threshold of nodes that should agree with each other
for your node to be convinced of something.

### Balancing safety and liveness
When configuring it, you want to balance safety and liveness:
a threshold of 100% will obviously guarantee that your node will agree with all
nodes in your quorum set at all time but if any of those nodes fails your
node will be stuck until all nodes come back and agree.
On the other hand, a threshold too low may cause the node to follow a broken minority.

### THRESHOLD_PERCENT and the "3f+1 rule"
One thing to keep in mind is that more validators doesn't translate
necessarily to better resilience as the number of byzantine failures `f`
 is linked to the number of nodes `n` by `n>=3f+1`.
The implication is that a 4 nodes network can only handle one byzantine
failure (f=1).
If you add 2 nodes (bringing the network to 6 nodes), it can still only handle
one failure (2 failures requires 7 nodes).

### Quorum intersection
As each quorum set refers to other nodes (that themselves have a quorum set
 configured), the overall network will reach consensus using this graph of
 nodes.
Particular attention has to be made to ensure that the overall network has
what is called "quorum intersection":
no two distinct sets of nodes should be allowed to agree to something
 different. It's similar to what happens on a network that is fully
 partitioned where for example, a DNS request could yield completely
 different results depending on which partition you're talking to.
The easiest way to ensure that is that all nodes should have a large overlap
in how they configured their quorum set. Just like having redundant paths
between machines in a network increases the reliability of the network.
Overlap here means that any two nodes that reference a set of nodes:
 * have a large overlap of the nodes
 * the threshold is such that there will always be some overlap between nodes
   regardless of which node fails

For example, consider two nodes that respectively reference the sets Set1 and
 Set2 composed of some common nodes and some other nodes.

 * Set1 = Common + extra1
 * Set2 = Common + extra2

Then if you want to ensure that when reaching consensus, each node has
at least "safety" number of nodes in common.
 * threshold1 >= (size(extra1) + safety)/size(Set1)
 * threshold2 >= (size(extra2) + safety)/size(Set2)

This can be expressed in percentage:
 * safetyP = safety/common * 100
 * commonP = common/sizeof(SetN) * 100
 
threshold then should be greater or equal to
 * 100 - commonP + (safetyP*commonP)/100   or 
 * 100 - (1 - safetyP/100)*commonP

so if 80% of the nodes in the set are common, and you consider that seeing 60% of those is enough
threshold should be set to 100 - 80 + 60*80/100 = 68

### Picking validators
You want to pick validators that are reliable:
 * they are available (not crashed/down often)
 * they follow the protocol (configured properly, not buggy, not malicious)

You do not need to put a lot of nodes there, just the ones that you think
 are the most representative of the network. Of course, as you increase the
 number of validators in your quorum set (at constant threshold), your node
 will be more resilient to validator failures.

 A good starting point is to pick all validators from the network and
 remove over time the ones that are causing you problems.

Other node operators may or may not chose the same validators than you, it's
up to them! All you need is to have a good overlap across the population of
 validators.

### Typical way to configure QUORUM_SET

If you are running a single node, your best bet is to configure your QUORUM_SET
organized such that the validators you consider "stable" are at the top of
the list "Stable", and other nodes that you are not ready yet to fully relly
 on, but that you are considering for your stable list under various "Trial"
 groups. Individual trial groups have the same weight than a single stable
 validator when configured as below.

 ```
[QUORUM_SET]
THRESHOLD_PERCENT=70 # optional, the default is fine
# "Stable validators" 
VALIDATORS=[
"sdf1",
"sdf2",
"sdf3",
...
]
# Nodes that are being considered for inclusion
[QUORUM_SET.trial_group_1]
THRESHOLD_PERCENT=51
VALIDATORS=[
"someNewValidator1",
"someNewValidator2",
"someNewValidator3"
]
[QUORUM_SET.trial_group_2]
VALIDATORS=[
"someNewValidator4"
]
```

 Be sure to not add too many "trial" groups at the top level:
 too many non reliable groups at the top level are a threat
 to liveness and may cause your node to get stuck.
 If you have too many top level "trial" groups, you may have to group "trial"
 groups into hierarchies of "trial" groups in order to keep the top level
 reliable.

 How many "trial" groups you can add depends on the level of risk you're
 willing to take:
 your "stable" group should represent a figure above the threshold that
 you picked (and the threshold should be 66% for the top level).

 Example:
 If you have 3 nodes in your "stable" list, you may have one node in your "trial" group
 but consider that in this case you cannot tolerate any failure from your "stable" set
 (as you would only have 2 nodes out of 4).
 Once you have 6 nodes in your "stable" list you can handle a failure of
 one of your "stable" list and one failure in the "trial" list.

 As you can see, in order to bootstrap a public network, you will need at least
6 "stable" nodes in order to start vetting other nodes over time.

### Advanced QUORUM_SET configuration
A more advanced way to configure your QUORUM_SET is to group validators by
 entity type or organization.
For example, you can imagine a QUORUM_SET that looks like
```
[t: 100,
    [ t: 66, bank-1, bank-2, bank-3 ], # banks
    [ t: 51, sdf, foundation-1, ...  ], # nonprofits
    [ t: 51, univ-1, ... ], # universities
    [ t: 51, friend-1, ... ] # friends
]
```
or more exactly, as entities are represented by validators
```
[t: 100, # requires all entities to be present
    [ t: 66, # super majority of banks
       [t: 66, bank1-server1, bank1-server2, bank1-server3],
       [t: 51, bank2-server1, bank2-server2],
       [t: 100, bank3-server1]
    ],
    [ t: 51, # majority of nonprofits
       [t: 51, sdf1, sdf2, sdf3],
       [t: 51, foundation-1-server1, ...],
       ...
    ],
    [ t: 51, # majority of universities
        ...
    ],
    [ t: 51, # majority of friends
        ...
    ]
]
```

This will configure the node to require one node of each category to reach consensus.

# Recipes

## Joining an existing network

Put the network's `KNOWN_PEERS`, `QUORUM_SET`, and `HISTORY` details in a config file.
Optionally: If you're going to be a validating node, generate key pair and 
set `NODE_SEED` to your seed, and `NODE_IS_VALIDATOR=true`.
Optionally: Create an external database to use--e.g., by using 
PostgreSQL's `createdb` command.
Set the `DATABASE` config variable to your choice of database.

Run:

1. `$ stellar-core --newdb`
  - if you need to initialize the database
2. `$ stellar-core`
  - to start the node

## Starting a new network

Generate a keypair for each node, and set `NODE_SEED` and `NODE_IS_VALIDATOR=true`
on each node of your new network.
Set the `QUORUM_SET` and `KNOWN_PEERS` of each node to refer to one another.
Decide on a history archive and add a HISTORY config entry for it on each node.
Optionally: Create databases for each to use--e.g., by using PostgreSQL's `createdb` command.
Set the `DATABASE` config variables on each node to your choice of database.

Run:

1. `$ stellar-core --newhist <historyarchive>`
  - to initialize every history archive you are putting to (be sure to not push to the same archive from different nodes).
2. `$ stellar-core --newdb`
  - to initialize the database on each node. 
3. `$ stellar-core --forcescp`
  - to set a flag to force each node to start SCP immediatly rather than wait to hear from the network. 
4. `$ stellar-core` 
  - on each node to start it.

## Upgrading network settings

Read the section on [`network-configuration`](admin.md#network-configuration) for process to follow.

Example here is to upgrade the protocol version to version 9 on January-31-2018.

1. `$ stellar-core -c 'upgrades?mode=set&upgradetime=2018-01-31T20:00:00Z&protocolversion=9'`

2. `$ stellar-core -c info`
At this point `info` will tell you that the node is setup to vote for this upgrade:
```
      "status" : [
         "Armed with network upgrades: upgradetime=2018-01-31T20:00:00Z, protocolversion=9"
      ]
```

# Understanding the availability and health of your instance
## General info
Run `$ stellar-core --c 'info'`
The output will look something like
```
   "info" : {
      "build" : "v0.2.3-9-g73147b7",
      "ledger" : {
         "age" : 6,
         "closeTime" : 1446178539,
         "hash" : "f3c3424b85c004ebea1ae25991cf2ff902b46a5fea3bce1850c032118cd4567c",
         "num" : 474367
      },
      "network" : "Public Global Stellar Network ; September 2015",
      "numPeers" : 12,
      "protocol_version" : 1,
      "quorum" : {
         "474366" : {
            "agree" : 5,
            "disagree" : 0,
            "fail_at" : 2,
            "hash" : "ac8c66",
            "missing" : 0,
            "phase" : "EXTERNALIZE"
         }
      },
      "state" : "Synced!"
```
Key fields to watch for:
 * `state` : should be "Synced!"
 * `ledger age`: when was the last ledger closed, should be less than 10 seconds
 * `numPeers` : number of peers connected
 * `quorum` : summary of the quorum information for this node (see below)

## Quorum Health
Run `$ stellar-core --c 'quorum'`
The output looks something like
```
"474313" : {
         "agree" : 6,
         "disagree" : null,
         "fail_at" : 2,
         "fail_with" : [ "lab1", "lab2" ],
         "hash" : "d1dacb",
         "missing" : [ "donovan" ],
         "phase" : "EXTERNALIZE",
         "value" : {
            "t" : 5,
            "v" : [ "lab1", "lab2", "lab3", "donovan", "GDVFV", "nelisky1", "nelisky2" ]
         }
```
The key entries to watch are:
  * `value` : the quorum set used by this node.
  * `agree` : the number of nodes in the quorum set that agree with this instance.
  * `disagree`: the nodes that were participating but disagreed with this instance.
  * `missing` : the nodes that were missing during this consensus round.
  * `fail_at` : the number of failed nodes that would cause this instance to halt.
  * `fail_with`: an example of such failure.
In the example above, 6 nodes are functioning properly, one is down (`donovan`), and
 the instance will fail if any two nodes out of the ones still working fail as well.

Note that the node not being able to reach consensus does not mean that the network
as a whole will not be able to reach consensus (and the opposite is true, the network
may fail because of a different set of validators failing).

You can get a sense of the quorum set health of a different node by doing
`$ stellar-core --c 'quorum?node=$sdf1` or `$ stellar-core --c 'quorum?node=@GABCDE` 

You can get a sense of the general health of the network by looking at the quorum set
health of all nodes.

# Validator maintenance

Maintenance here refers to anything involving taking your validator temporarily out of
the network (to apply security patches, system upgrade, etc).

As an administrator of a validator, you must ensure that the maintenance you are
about to take on a validator is safe for the overall network and for your validator.
Safe means that the other validators that depend on yours will not be affected
too much when you turn off your validator for maintenance and that your validator
will continue to operate as part of the network when it comes back up.

If you are changing some settings that may impact network wide settings, such as
upgrading to a new version of stellar-core that supports a new version of the
protocol or if you're updating other network wide settings such , review the
section "Important notes on network wide settings".

We recommend performing the following steps in order (once per machine if you
 run multiple nodes).

1. Advertise your intention to others that may depend on you. Some coordination
 is required to avoid situations where too many nodes go down at the same time.
2. Dependencies should assess the health of their quorum, refer to the section
 "Understanding quorum and reliability".
3. If there is no objection, take your instance down
4. When done, start your instance that should rejoin the network
5. The instance will be completely caught up when it's both `Synced` and there
 is no backlog in uploading history.

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
