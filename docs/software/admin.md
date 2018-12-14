---
title: Administration
---

## Purpose of this document

This document describes various aspects of running `stellar-core` for **system administrators** (but may be useful to a broader audience).

## Introduction

Stellar Core is responsible for communicating directly with and maintaining the Stellar peer-to-peer network. For a high-level introduction to Stellar Core, [watch this talk](https://www.youtube.com/watch?v=pt_mm8S9_WU) on the architecture and ledger basics:

[![Introduction to Stellar Core](https://i.ytimg.com/vi/pt_mm8S9_WU/hqdefault.jpg "Introduction to Stellar Core")](https://www.youtube.com/watch?v=pt_mm8S9_WU)

It will also be useful to understand how [data flows](https://www.stellar.org/developers/stellar-core/software/core-data-flow.pdf) and is stored in the system.

## Zero to completed: node checklist
 - [ ] [deciding to run a node](#why-run-a-node)
 - [ ] [setting up an instance to run core](#instance-setup)
 - [ ] [install stellar-core](#installing)
 - [ ] [craft a configuration](#configuring)
 - [ ] [crafting  a quorum set](#crafting-a-quorum-set)
 - [ ] [preparing the environment before the first run](#environment-preparation)
 - [ ] [joining the network](#joining-the-network)
 - [ ] [logging](#logging)
 - [ ] [monitoring and diagnostics](#monitoring-and-diagnostics)
 - [ ] [performing validator maintenance](#validator-maintenance)
 - [ ] [performing network wide updates](#network-configuration)
 - [ ] [advanced topics and internals](#advanced-topics-and-internals)

## Why run a node?

### Benefits of running a node

You get to run your own Horizon instance:
* Allows for customizations (triggers, etc) of the business logic or APIs
* Full control of which data to retain (historical or online)
* A trusted entry point to the network
  * Trusted end to end (can implement additional counter measures to secure services)
  * Open Horizon increases customer trust by allowing to query at the source (ie: larger token issuers have an official endpoint that can be queried)
* Control of SLA

note: in this document we use "Horizon" as the example implementation of a first tier service built on top of stellar-core, but any other system would get the same benefits.

### Level of participation to the network

As a node operator you can participate to the network in multiple ways.

|                                                    | watcher       | archiver                            | basic validator                                                                                                             | full validator                       |
| -------------------------------------------------- | ------------- | ----------------------------------- | --------------------------------------------------------------------------------------------------------------------------- | ------------------------------------ |
| description                                        | non-validator | all of watcher + publish to archive | all of watcher + active participation in consensus (submit proposals for the transaction set to include in the next ledger) | basic validator + publish to archive |
| submits transactions                               | yes           | yes                                 | yes                                                                                                                         | yes                                  |
| supports horizon                                   | yes           | yes                                 | yes                                                                                                                         | yes                                  |
| participates in consensus                          | no            | no                                  | yes                                                                                                                         | yes                                  |
| helps other nodes to catch up and join the network | no            | yes                                 | no                                                                                                                          | yes                                  |
| Increase the resiliency of the network             | No            | Medium                              | Low                                                                                                                         | High                                 |

From an operational point of view "watchers" and "basic validators" are about the
 same (they both compute an up to date version of the ledger).
"Archivers" or "Full validators" publish into an history archive which
 has additional cost.

#### Watcher nodes

Watcher nodes are configured to watch the activity from the network

Use cases:
* Ephemeral instances, where having other nodes depend on those nodes is not desired
* Potentially reduced administration cost (no or reduced SLA)
* Real time network monitoring (which validators are present, etc)
* Generate network meta-data for other systems (Horizon) 

**Operational requirements**:
* a [database](#database)

#### Archiver nodes

The purpose of Archiver nodes is to record the activity of the network in long term storage (AWS, Azure, etc).

[History Archives](#history-archives) contain snapshots of the ledger, all transactions and their results.

Use cases:
* Everything that a watcher node can do
* Need for a low cost compliance story
* Participate to the network’s resiliency
* Analysis of historical data

**Operational requirements**:
* requires an additional internet facing blob store
* a [database](#database)

#### Basic validators
Nodes configured to actively vote on the network.

Use cases:
* Everything that a watcher node can do
* Increase the network reliability
* Enables deeper integrations by clients and business partners
* Official endorsement of specific ledgers in real time (via signatures)
* Quorum Set aligned with business priorities
* Additional checks/invariants enabled
  * Validator can halt and/or signal that for example (in the case of an issuer) that it does not agree to something.

**Operational requirements**: 
* secret key management (used for signing messages on the network)
* a [database](#database)

#### Full validators

Nodes fully participating in the network.

Full validators are the true measure of how decentralized and redundant the network is as they are the only type of validators that perform all functions on the network.

Use cases:
* All other use cases
* Some full validators required to be v-blocking (~ N full validators, M other validators on the network -> require at least M+1 threshold)
* Branding - strongest association with the network
* Mutually beneficial - best way to support the network’s health and resilience

**Operational requirements**:
* requires an additional internet facing blob store 
* secret key management (used for signing messages on the network)
* a [database](#database)

## Instance setup
Regardless of how you install stellar-core (apt, source, docker, etc), you will need to configure the instance hosting it roughly the same way.

### Compute requirements
CPU, RAM, Disk and network depends on network activity. If you decide to collocate certain workloads, you will need to take this into account.

As of early 2018, stellar-core with PostgreSQL running on the same machine has no problem running on a [m5.large](https://aws.amazon.com/ec2/instance-types/m5/) in AWS (dual core 2.5 GHz Intel Xeon, 8 GB RAM).

Storage wise, 20 GB seems to be an excellent working set as it leaves plenty of room for growth.

### Network access

#### Interaction with the peer to peer network
* **inbound**: stellar-core needs to allow all ips to connect to its `PEER_PORT` (default 11625) over TCP.
* **outbound**: stellar-core needs access to connect to other peers on the internet on `PEER_PORT` (most use the default as well) over TCP.

#### Interaction with other internal systems

* **outbound**:
  * stellar-core needs access to a database (postgresql for example), which may reside on a different machine on the network
  * other connections can safely be blocked
* **inbound**: stellar-core exposes an *unauthenticated* HTTP endpoint on port `HTTP_PORT` (default 11626)
  * it is used by other systems (such as Horizon) to submit transactions (so may have to be exposed to the rest of your internal ips)
  *  query information (info, metrics, ...) for humans and automation
  *  perform administrative commands (schedule upgrades, change log levels, ...)

Note on exposing the HTTP endpoint:
if you need to expose this endpoint to other hosts in your local network, it is recommended to use an intermediate reverse proxy server to implement authentication. Don't expose the HTTP endpoint to the raw and cruel open internet.

## Installing

### Release version

In general you should aim to run the latest [release](https://github.com/stellar/stellar-core/releases) as builds are backward compatible and are cummulative.

The version number scheme that we follow is `protocol_version.release_number.patch_number`, where
* `protocol_version` is the maximum protocol version supported by that release (all versions are 100% backward compatible),
* `release_number` is bumped when a set of new features or bug fixes not impacting the protocol are included in the release,
* `patch_number` is used when a critical fix has to be deployed 

### Installing from source
See the [INSTALL](https://github.com/stellar/stellar-core/blob/master/INSTALL.md) for build instructions.

### Package based Installation
If you are using Ubuntu 16.04 LTS we provide the latest stable releases of [stellar-core](https://github.com/stellar/stellar-core) and [stellar-horizon](https://github.com/stellar/go/tree/master/services/horizon) in Debian binary package format.

See [detailed installation instructions](https://github.com/stellar/packages#sdf---packages)

### Container based installation
Docker images are maintained in a few places, good starting points are:
 * the [quickstart image](https://github.com/stellar/docker-stellar-core-horizon)
 * the [standalone image](https://github.com/stellar/docker-stellar-core). **Warning**: this only tracks the latest master, so you have to find the image based on the [release](https://github.com/stellar/stellar-core/releases) that you want to use.

## Configuring

Before attempting to configure stellar-core, it is highly recommended to first try running a private network or joining the test network. 

### Configuration basics
All configuration for stellar-core is done with a TOML file. By default 
stellar-core loads `./stellar-core.cfg`, but you can specify a different file to load on the command line:

`$ stellar-core --conf betterfile.cfg <COMMAND>`

The [example config](https://github.com/stellar/stellar-core/blob/master/docs/stellar-core_example.cfg) is not a real configuration, but documents all possible configuration elements as well as their default values.

Here is an [example test network config](https://github.com/stellar/docker-stellar-core-horizon/blob/master/testnet/core/etc/stellar-core.cfg) for connecting to the test network.

Here is an [example public network config](https://github.com/stellar/docs/blob/master/other/stellar-core-validator-example.cfg) for connecting to the public network.

The examples in this file don't specify `--conf betterfile.cfg` for brevity.

### Validating node
Nodes are considered **validating** if they take part in SCP and sign messages 
pledging that the network agreed to a particular transaction set. It isn't 
necessary to be a validator. Only set your node to validate if other nodes 
care about your validation.

If you want to validate, you must generate a public/private key for your node.
 Nodes shouldn't share keys. You should carefully *secure your private key*. 
If it is compromised, someone can send false messages to the network and those 
messages will look like they came from you.

Generate a key pair like this:

`$ stellar-core gen-seed`
the output will look something like
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

### Crafting a quorum set

This section describes how to configure the quorum set for a validator and assumes basic understanding of the [Stellar Consensus Protocol](https://www.stellar.org/developers/guides/concepts/scp.html).

#### Validator list

You will find lists of validators in a few places:
* [list of validators](https://github.com/stellar/docs/blob/master/validators.md)
* the [Stellar Dashboard](https://dashboard.stellar.org/)

#### Understanding requirements for a good quorum

The way quorum sets are configured is explained in detail in the [example config](https://github.com/stellar/stellar-core/blob/master/docs/stellar-core_example.cfg).

As an administrator what you need to do is ensure that your quorum configuration:
* is aligned with how you want to trust other nodes on the network
* gives good guarantees on the quorum intersection property of the network
* provides the right properties in the event of arbitrary node failures

If you are running multiple validators, the availability model of your organization as a "group of validators" (the way people are likely to refer to your validators) is not like traditional web services:
* traditional web services stay available down to the last node
* in the consensus world, for your group to be available, 67% of your nodes have to agree to each other

#### Recommended pattern for building a quorum set

Divide the validators into two categories:
* [full validators](#full-validators)
* [basic validators](#basic-validators) 

One of the goals is to ensure that there will always be some full validators in any given quorum (from your node's point of view).

As the way quorum sets are specified using a threshold, i.e. require T out of N entities (groups or individual validators) to agree, the desired property is achieved by simply picking a threshold at least equal to the number of basic entities at the top level + 1.

```toml
[QUORUM_SET]
THRESHOLD_PERCENT= ?
VALIDATORS= [ ... ]

# optional, other full validators grouped by entity
[QUORUM_SET.FULLSDF]
THRESHOLD_PERCENT= 66
VALIDATORS = [ ... ]

# other basic validators
[QUORUM_SET.BASIC]
THRESHOLD_PERCENT= ?
VALIDATORS= [ ... ]

# optional, more basic validators from entity XYZ
[QUORUM_SET.BASIC.XYZ]
THRESHOLD_PERCENT= 66
VALIDATORS= [ ... ]
```

A simple configuration with those properties could look like this:
```toml
[QUORUM_SET]
# this setup puts all basic entities into one top level one
# this makes the minimum number of entities at the top level to be 2
# with 3 validators, we then end up with a minimum of 50%
# more would be better at the expense of liveness in this example
THRESHOLD_PERCENT= 67
VALIDATORS= [ "$sdf1", "$sdf2", "$sdf3" ]

[QUORUM_SET.BASIC]
THRESHOLD_PERCENT= 67
VALIDATORS= [ ... ]

[QUORUM_SET.BASIC.XYZ]
THRESHOLD_PERCENT= 67
VALIDATORS= [ ... ]
```

#### Picking thresholds

Thresholds and groupings go hand in hand, and balance:
 * liveness - network doesn't halt when some nodes are missing (during maintenance for example)
 * safety - resistance to bad votes, some nodes being more important (full validators) for the normal operation of the network

Liveness pushes thresholds lower and safety pushes thresholds higher.

On the safety front, ideally any group (regardless of its composition), can suffer a 33% byzantine failure, but in some cases this is not practical and a different configuration needs to be picked.

You may have to change the grouping in order to achieve the expected properties:
* merging groups typically makes the group more resilient, compare:
  * [51%, [51%, A, B, C, D], [51%, E, F, G, H]] # group of 4 has a threshold of 3 nodes -> 2 nodes missing enough to halt
  * [51%, A, B, C, D, E, F, G, H] # 8 nodes -> 5 nodes threshold -> 4 nodes missing to halt 
* splitting groups can also be useful to make certain entities optional, compare:
  * [100%, [51%, A, B, C], [50%, D, E]] # requires D or E to agree
  * [ 67%, A, B, C, [50%, D, E]] # D or E only required if one of A,B,C doesn't agree (the [D,E] group acts as a tie breaker)

#### Quorum and overlay network

It is generally a good idea to give information to your validator on other validators that you rely on. This is achieved by configuring `KNOWN_PEERS` and `PREFERRED_PEERS` with the addresses of your dependencies.

Additionally, configuring `PREFERRED_PEER_KEYS` with the keys from your quorum set might be a good idea to give priority to the nodes that allows you to reach consensus.

Without those settings, your validator depends on other nodes on the network to forward you the right messages, which is typically done as a best effort.

#### Special considerations during quorum set updates

Sometimes an organization needs to make changes that impact other's quorum sets:
* taking a validator down for long period of time
* adding new validators to their pool

In both cases, it's crucial to stage the changes to preserve quorum intersection and general good health of the network:
* removing too many nodes from your quorum set *before* the nodes are taken down : if different people remove different sets the remaining sets may not overlap between nodes and may cause network splits 
* adding too many nodes in your quorum set at the same time : if not done carefully can cause those nodes to overpower your configuration

Recommended steps are for the entity that adds/removes nodes to do so first between their own nodes, and then have people reflect those changes gradually (over several rounds) in their quorum configuration.

## Environment preparation

### stellar-core configuration
Cross reference your validator settings, in particular:
* environment specific settings
  * network passphrase
  * known peers
* quorum set
  * public keys of the validators that you manage grouped properly
* seed defined if validating
* [Automatic maintenance](#cursors-and-automatic-maintenance) configured properly, especially when stellar-core is used in conjunction with a downstream system like Horizon.

### Database and local state

After configuring your [database](#database) and [buckets](#buckets) settings, when running stellar-core for the first time, you must initialize the database:

`$ stellar-core new-db`

This command will initialize the database as well as the bucket directory and then exit. 

You can also use this command if your DB gets corrupted and you want to restart it from scratch. 

#### Database
Stellar-core stores the state of the ledger in a SQL database.

This DB should either be a SQLite database or, for larger production instances, a separate PostgreSQL server.

*Note: Horizon currently depends on using PostgreSQL.*

For how to specify the database, 
see the [example config](https://github.com/stellar/stellar-core/blob/master/docs/stellar-core_example.cfg).

##### Cursors and automatic maintenance

Some tables in the database act as a publishing queue for external systems such as Horizon and generate **meta data** for changes happening to the distributed ledger.

If not managed properly those tables will grow without bounds. To avoid this, a built-in scheduler will delete data from old ledgers that are not used anymore by other parts of the system (external systems included).

The settings that control the automatic maintenance behavior are: `AUTOMATIC_MAINTENANCE_PERIOD`,  `AUTOMATIC_MAINTENANCE_COUNT` and `KNOWN_CURSORS`.

By default, stellar-core will perform this automatic maintenance, so be sure to disable it until you have done the appropriate data ingestion in downstream systems (Horizon for example sometimes needs to reingest data).

If you need to regenerate the meta data, the simplest way is to replay ledgers for the range you're interested in after (optionally) clearing the database with `newdb`.

#### Buckets
Stellar-core stores a duplicate copy of the ledger in the form of flat XDR files 
called "buckets." These files are placed in a directory specified in the config 
file as `BUCKET_DIR_PATH`, which defaults to `buckets`. The bucket files are used
 for hashing and transmission of ledger differences to history archives. 

Buckets should be stored on a fast local disk with sufficient space to store several times the size of the current ledger. 
 
 For the most part, the contents of both directories can be ignored as they are managed by stellar-core.

### History archives
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

#### Configuring to publish to an archive
Archive sections can also be configured with `put` and `mkdir` commands to
 cause the instance to publish to that archive (for nodes configured as [archiver nodes](#archiver-nodes) or [full validators](#full-validators)).

The very first time you want to use your archive *before starting your node* you need to initialize it with:
`$ stellar-core new-hist <historyarchive>`

**IMPORTANT:**
 * make sure that you configure both `put` and `mkdir` if `put` doesn't
 automatically create sub-folders
 * writing to the same archive from different nodes is not supported and
 will result in undefined behavior, *potentially data loss*.
 * do not run `newhist` on an existing archive unless you want to erase it.

### Other preparation

In addition, your should ensure that your operating environment is also functional.

In no particular order:
* logging and log rotation
* monitoring and alerting infrastructure

## Starting your node

After having configured your node and its environment, you're ready to start stellar-core.

This can be done with a command equivalent to

`$ stellar-core run`

At this point you're ready to observe core's activity as it joins the network.

Review the [logging](#logging) section to get yourself familiar with the output of stellar-core.

### Interacting with your instance
While running, interaction with stellar-core is done via an administrative 
HTTP endpoint. Commands can be submitted using command-line HTTP tools such 
as `curl`, or by running a command such as

`$ stellar-core http-command <http-command>`

The endpoint is [not intended to be exposed to the public internet](#interaction-with-other-internal-systems). It's typically accessed by administrators, or by a mid-tier application to submit transactions to the Stellar network. 

See [commands](./commands.md) for a description of the available commands.

### Joining the network

You can review the section on [general node information](#general-node-information);

the node will go through the following phases as it joins the network:

#### Establish connection to other peers

You should see `authenticated_count` increase.

```json
"peers" : {
         "authenticated_count" : 3,
         "pending_count" : 4
      },
```

#### Observing consensus

Until the node sees a quorum, it will say
```json
"state" : "Joining SCP"
```

After observing consensus, a new field `quorum` will be set with information on what the network decided on, at this point the node will switch to "*Catching up*":
```json
      "quorum" : {
         "7667384" : {
            "agree" : 3,
            "disagree" : 0,
            "fail_at" : 2,
            "hash" : "273af2",
            "missing" : 0,
            "phase" : "EXTERNALIZE"
         }
      },
      "state" : "Catching up",
```

#### Catching up

This is a phase where the node downloads data from archives.
The state will start with something like
```json
      "state" : "Catching up",
      "status" : [ "Catching up: Awaiting checkpoint (ETA: 35 seconds)" ]
```

and then go through the various phases of downloading and applying state such as
```json
      "state" : "Catching up",
      "status" : [ "Catching up: downloading ledger files 20094/119803 (16%)" ]
```

#### Synced

When the node is done catching up, its state will change to
```json
      "state" : "Synced!"
```

## Logging
Stellar-core sends logs to standard output and `stellar-core.log` by default, 
configurable as `LOG_FILE_PATH`.

 Log messages are classified by progressive _priority levels_:
  `TRACE`, `DEBUG`, `INFO`, `WARNING`, `ERROR` and `FATAL`.
   The logging system only emits those messages at or above its configured logging level. 

The log level can be controlled by configuration, the `-ll` command-line flag 
or adjusted dynamically by administrative (HTTP) commands. Run:

`$ stellar-core http-command "ll?level=debug"`

against a running system.
Log levels can also be adjusted on a partition-by-partition basis through the 
administrative interface.
 For example the history system can be set to DEBUG-level logging by running:

`$ stellar-core http-command "ll?level=debug&partition=history"` 

against a running system.
 The default log level is `INFO`, which is moderately verbose and should emit 
 progress messages every few seconds under normal operation.


## Monitoring and diagnostics

Information provided here can be used for both human operators and programmatic access.

### General node information
Run `$ stellar-core http-command 'info'`
The output will look something like
```json
 {
   "info" : {
      "UNSAFE_QUORUM" : "UNSAFE QUORUM ALLOWED",
      "build" : "v9.2.0",
      "ledger" : {
         "age" : 1,
         "baseFee" : 100,
         "baseReserve" : 5000000,
         "closeTime" : 1519857801,
         "hash" : "c8e484c665cdb8280cc2923d1ead5277f6c31f5baab382f54b22c801e2c50a66",
         "num" : 7667629,
         "version" : 9
      },
      "network" : "Test SDF Network ; September 2015",
      "peers" : {
         "authenticated_count" : 3,
         "pending_count" : 5
      },
      "protocol_version" : 9,
      "quorum" : {
         "7667628" : {
            "agree" : 3,
            "disagree" : 0,
            "fail_at" : 2,
            "hash" : "273af2",
            "missing" : 0,
            "phase" : "EXTERNALIZE"
         }
      },
      "startedOn" : "2018-02-28T22:38:20Z",
      "state" : "Synced!"
   }
}
```

`peers` gives information on the connectivity to the network, `authenticated_count` are live connections while `pending_count` are connections that are not fully established yet.

`ledger` represents the local state of your node, it may be different from the network state if your node was disconnected from the network for example.

notable fields in ledger are:
* `age` : time elapsed since this ledger closed (during normal operation less than 10 seconds)
* `num` : ledger number
* `version` : protocol version supported by this ledger


The state of a fresh node (reset with `newdb`), will look something like this:
```json
"ledger" : {
         "age" : 1519857653,
         "baseFee" : 100,
         "baseReserve" : 100000000,
         "closeTime" : 0,
         "hash" : "63d98f536ee68d1b27b5b89f23af5311b7569a24faf1403ad0b52b633b07be99",
         "num" : 2,
         "version" : 0
      },
```

Additional fields typically used by downstream systems:
* `build` is the build number for this stellar-core instance
* `network` is the network passphrase that this core instance is connecting to
* `protocol_version` is the maximum version of the protocol that this instance recognizes

In some cases, nodes will display additional status information:

```json
      "status" : [
         "Armed with network upgrades: upgradetime=2018-01-31T20:00:00Z, protocolversion=9"
      ]
```
### Overlay information

The `peers` command returns information on the peers the instance is connected to.

This list is the result of both inbound connections from other peers and outbound connections from this node to other peers.

`$ stellar-core http-command 'peers'`

```json
{
   "authenticated_peers" : [
      {
         "id" : "sdf2",
         "ip" : "54.211.174.177",
         "olver" : 5,
         "port" : 11625,
         "ver" : "v9.1.0"
      },
      {
         "id" : "sdf3",
         "ip" : "54.160.175.7",
         "olver" : 5,
         "port" : 11625,
         "ver" : "v9.1.0"
      },
      {
         "id" : "sdf1",
         "ip" : "54.161.82.181",
         "olver" : 5,
         "port" : 11625,
         "ver" : "v9.1.0"
      }
   ],
   "pending_peers" : null
}
```

### Quorum set Health

The `quorum` command allows to diagnose problems with the quorum set of the local node.

Run

`$ stellar-core http-command 'quorum'`

The output looks something like:
```json
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

Entries to watch for are:
  * `agree` : the number of nodes in the quorum set that agree with this instance.
  * `disagree`: the nodes that were participating but disagreed with this instance.
  * `fail_at` : the number of failed nodes that *would* cause this instance to halt.
  * `fail_with`: an example of such potential failure.
  * `missing` : the nodes that were missing during this consensus round.
  * `value` : the quorum set used by this node (`t` is the threshold expressed as a number of nodes).

In the example above, 6 nodes are functioning properly, one is down (`donovan`), and
 the instance will fail if any two nodes out of the ones still working fail as well.

If a node is stuck in state `Joining SCP`, this command allows to quickly find the reason:
* too many validators missing (down or without a good connectivity), solutions are:
  * [adjust quorum set](#crafting-a-quorum-set) (thresholds, grouping, etc) based on the nodes that are not missing
  * try to get a [better connectivity path](#quorum-and-overlay-network) to the missing validators

* network split would cause SCP to be stuck because of nodes that disagree. This would happen if either there is a bug in SCP, the network does not have quorum intersection or the disagreeing nodes are misbehaving (compromised, etc)

Note that the node not being able to reach consensus does not mean that the network
as a whole will not be able to reach consensus (and the opposite is true, the network
may fail because of a different set of validators failing).

You can get a sense of the quorum set health of a different node by doing
`$ stellar-core http-command 'quorum?node=$sdf1` or `$ stellar-core http-command 'quorum?node=@GABCDE` 

Overall network health can be evaluated by walking through all nodes and looking at their health. Note that this is only an approximation as remote nodes may not have received the same messages (in particular: `missing` for other nodes is not reliable).

## Validator maintenance

Maintenance here refers to anything involving taking your validator temporarily out of the network (to apply security patches, system upgrade, etc).

As an administrator of a validator, you must ensure that the maintenance you are about to apply to the validator is safe for the overall network and for your validator.

Safe means that the other validators that depend on yours will not be affected too much when you turn off your validator for maintenance and that your validator will continue to operate as part of the network when it comes back up.

If you are changing some settings that may impact network wide settings such as protocol version, review [the section on network configuration](#network-configuration).

If you're changing your quorum set configuration, also read the [section on what to do](#special-considerations-during-quorum-set-updates).

### Recommended steps to perform as part of a maintenance

We recommend performing the following steps in order (repeat sequentially as needed if you run multiple nodes).

1. Advertise your intention to others that may depend on you. Some coordination is required to avoid situations where too many nodes go down at the same time.
2. Dependencies should assess the health of their quorum, refer to the section
 "Understanding quorum and reliability".
3. If there is no objection, take your instance down
4. When done, start your instance that should rejoin the network
5. The instance will be completely caught up when it's both `Synced` and *there is no backlog in uploading history*.

## Network configuration

The network itself has network wide settings that can be updated.

This is performed by validators voting for and agreeing to new values the same way than consensus is reached for transaction sets, etc.

A node can be configured to vote for upgrades using the `upgrades` endpoint . see [`commands.md`](commands.md) for more information.

The network settings are:
 * the version of the protocol used to process transactions
 * the maximum number of transactions that can be included in a given ledger close
 * the cost (fee) associated with processing operations
 * the base reserve used to calculate the lumen balance needed to store things in the ledger

When the network time is later than the `upgradetime` specified in
the upgrade settings, the validator will vote to update the network
to the value specified in the upgrade setting.

When a validator is armed to change network values, the output of `info` will contain information about the vote.

For a new value to be adopted, the same level of consensus between nodes needs to be reached as for transaction sets.

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

### Example upgrade command

Example here is to upgrade the protocol version to version 9 on January-31-2018.

1. `$ stellar-core http-command 'upgrades?mode=set&upgradetime=2018-01-31T20:00:00Z&protocolversion=9'`

2. `$ stellar-core http-command info`
At this point `info` will tell you that the node is setup to vote for this upgrade:
```json
      "status" : [
         "Armed with network upgrades: upgradetime=2018-01-31T20:00:00Z, protocolversion=9"
      ]
```

## Advanced topics and internals

This section contains information that is useful to know but that should not stop somebody from running a node.

### Creating your own private network

[testnet.md](./testnet.md) is a short tutorial demonstrating how to
  configure and run a short-lived, isolated test network.

### Runtime information: start and stop

Stellar-core can be started directly from the command line, or through a supervision 
system such as `init`, `upstart`, or `systemd`.

Stellar-core can be gracefully exited at any time by delivering `SIGINT` or
 pressing `CTRL-C`. It can be safely, forcibly terminated with `SIGTERM` or
  `SIGKILL`. The latter may leave a stale lock file in the `BUCKET_DIR_PATH`,
   and you may need to remove the file before it will restart. 
   Otherwise, all components are designed to recover from abrupt termination.

Stellar-core can also be packaged in a container system such as Docker, so long 
as `BUCKET_DIR_PATH` and the database are stored on persistent volumes. For an
example, see [docker-stellar-core](https://github.com/stellar/docker-stellar-core-horizon).

### In depth architecture

[architecture.md](https://github.com/stellar/stellar-core/blob/master/docs/architecture.md) 
  describes how stellar-core is structured internally, how it is intended to be 
  deployed, and the collection of servers and services needed to get the full 
  functionality and performance.
