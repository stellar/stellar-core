---
title: Performance
---

## Purpose of this document

This document describes performance considerations for administrators and developers of `stellar-core`. It may also be interesting for a broader audience interested in understanding system performance and scaling limits and avenues for future work.

## Overview

The performance of a `stellar-core` node varies in two main dimensions:

  1. _What it is configured to do_:

     * As discussed in the [admin.md](./admin.md) file, nodes may be configured as watchers, archivers, basic validators or full validators. These roles have different performance costs.
     * Each such role may have a variety of options enabled or disabled at varying costs to the node.

  2. _How it is physically and logically configured to do its job_:

     * The physical hardware -- disks, CPUs, memory and networking equipment -- supporting the node can substantially affect performance.
     * Choices of software, server proximity and latency, networking topology and so forth can also substantially affect performance.

This document will begin with a brief structural review of subsystems of `stellar-core` and their performance considerations. Then for each of the two dimensions above, discuss it from technical-design perspective, then contain empirical advice from operators of existing `stellar-core` nodes, and finally note some areas where the current developers anticipate doing future performance work.

## Review of subsystems

For purposes of understanding performance, the following subsystems of `stellar-core` are worth keeping in mind:

  1. **Overlay**: This subsystem sends, receives, and repeats broadcast messages -- transactions in progress and consensus rounds -- from other nodes. It maintains many simultaneous connections and attempts to strike a balance between sending "too many" messages (at risk of overloading resources), and "too few" (at risk of delaying or even blocking consensus).
  2. **SCP**: This subsystem orchestrates the federated votes for nomination and commitment to a given transaction set that occurs at each ledger-close. It is responsible for generating (and receiving) much of the traffic seen in the overlay subsystem.
  3. **Herder**: This subsystem mediates the relationship between **overlay** and **SCP**, collecting, requesting and fulfilling dependencies of each step in **SCP** and responding to such requests from other nodes.
  4. **Transactions**: This subsystem evaluates the validity of each transaction during pre-consensus flooding, and then applies each consensus set of transactions to the ledger. It therefore generates a moderate CPU load in terms of signature and transaction-semantics verification most of the time, and then a spike of significant CPU and **database** load (via the **ledger**) once every 5 seconds, during ledger-close.
  5. **Ledger**: This subsystem reads and writes to and from the **database** the set of _ledger entries_ modified in a given ledger, calculates each new ledger header, emits changed entries to the **bucket list**, and queues records for publication through **history**. Roughly speaking, **transactions** generate performance load on **ledger** which generates performance load on **database** and **bucket list**. 
  6. **Database**: This subsystem is a small wrapper (with some caches and connection pooling) around SQLite or PostgreSQL. It receives load almost exclusively from **ledger**.
  7. **History**: This subsystem is responsible for queueing outgoing history records being sent to a _history archive_, as well as fetching and applying history records _from_ such an archive if the node falls out of sync with its peers and needs to catch up. Usually history is lightly loaded, but when it is active it can generate load on CPU and network, as well as against **ledger** (and thereby **database**).
  8. **Bucket list**: This subsystem maintains a redundant copy of the database in a log-structured merge tree form on disk, that is amenable to efficient delta calculation and cryptographic hashing. The former is used for "fast" catch-up, the latter for calculating a state-of-the-world hash at each ledger close. Buckets in the bucket list are written to disk and rewritten at exponentially-distributed intervals, with less-frequently-written buckets being commensurately exponentially larger. Thus periodic spikes in IO load will occur when larger buckets are (re)written; this happens on background threads and is optimized to be mostly-sequential, but it is still noticeable.

For more detail on these subsystems, see the [architecture.md](../architecture.md) document.

## Variation in node function

### Consensus dimension and quorum size

Nodes that are participating in consensus -- basic and full validators -- will typically cause higher load on the overlay, herder and SCP subsystems than nodes _not_ participating in consensus, especially if they are themselves relied-on in many other nodes' quorum sets. This load is usually in the form of bi-directional network traffic and CPU usage.

Furthermore, configuring a node with a larger quorum set -- whether the node is participating in consensus or not -- will typically cause higher load on the overlay, herder and SCP subsystems, and thus network and CPU usage.

Finally, regardless of consensus participation, the numerous `*_CONNECTIONS` parameters that govern the overlay subsystem's connection management strategy will influence load on overlay directly, and herder and SCP indirectly through increased broadcast traffic (even if mostly redundant). The general rule is: more connections means more load, though connections do need to be kept numerous enough to maintain low latency and connectivity.

Consensus-participation (or lack thereof) will typically _not_ alter the load caused on the transaction, ledger, database, history or bucket list subsystems: every node that is even _watching_ the network will apply all the transactions to the ledger, so needs an equally performant database.

A caveat to the previous paragraph is that if the SCP, herder or overlay subsystems are overloaded, a node may fall out of sync with its peers, which will then cause a load spike on the history subsystem (and subsequently on transaction, ledger, database and bucket list subsystems) as it initiates catchup.

### History archival dimension

Nodes that are saving historical records to a history archive will incur higher load on the history subsystem. This load is usually in the form of temporary disk space on the archiving node while queueing checkpoints, temporary spikes in _outgoing_ network traffic every 5 minutes (as checkpoints are formed), minor CPU overhead while running upload processes (up to `MAX_CONCURRENT_SUBPROCESSES` worth), and long-term continuous growth of (relatively cold) storage use in the archive.

The performance of each such process will vary depending on how it is configured: typically a `get` or `put` command for a history archive will just be an invocation of `curl` or `aws` or such command-line tool, so you should ensure that your node has adequate memory and CPU to run `MAX_CONCURRENT_SUBPROCESSES` worth of that tool, whatever it is.

History archiving (or lack thereof) will typically _not_ alter the load caused on _any other_ subsystems. Since history archiving is essential to having adequate backups of the state of the network, it is strongly recommended that nodes archive history if they can afford to.

### Horizon support dimension

Nodes configured to support a Horizon server are typically under significant load at the database level _caused by_ Horizon. This means that the resulting database performance available to `stellar-core`'s database, ledger and transaction subsystems is greatly reduced. Configuring dedicated non-consensus nodes to support Horizon, or running Horizon on a read replica of a `stellar-core` database, may be a reasonable option to alleviate this contention.

## Variation in node configuration

### Physical hardware configuration

Several key steps in the `stellar-core` operating loop latency-sensitive: it will fail to keep up / fall out of sync if it cannot complete various actions in a timely enough fashion. It is therefore important to the wellbeing of the server that several physical dimensions be selected for low-latency:

  1. Disks storing the database and buckets -- on any node -- should be SSD or NVMe if possible. Neither of these are particularly large data stores -- they only store the _active_ state of the system, not its entire history -- but they are consulted in relatively latency-critical ways during ledger-close.
  2. For nodes participating in consensus, network links between a node and its _quorum-slice peers_ should be as low-latency as possible, including situating them within the same region or datacenter. This will of course be in some conflict with fault-tolerance, so don't overdo it; but lower latency will generally result in faster and more stable consensus voting.
  3. For archiving nodes, there is no particular need to serve at high performance or use expensive storage hardware, especially if you are not exposing it to others for public use. If operating a private archive, it is fine to use a slower "infrequent-access" tiered storage system.

### Logical / software configuration

Some key configuration choices concerning storage access will greatly affect performance:

  1. The `BUCKET_DIR_PATH` config option sets the location that `stellar-core` places its buckets while (re)writing the bucket list. This should be located on a relatively fast, low-latency local disk. Ideally SSD or NVMe or similar. The faster the better. It does not need to be _very_ large and should not grow in usage _very_ fast, though `stellar-core` will fail if it fills up, so keep an eye on its utilization and make sure there's plenty of room.
  2. The `DATABASE` config value controls not only which _kind_ of database the node is performing transactions against, but also _where_ the database is located. Unlike with many database-backed programs, the _content_ of the database in a `stellar-core` installation is somewhat ephemeral: every node has a complete copy of it, as does every history archive, and the database can always be restored / rebuilt from history archives (it is in fact being continuously backed up every 5 minutes). So the main thing to optimize for here is latency, especially on nodes doing consensus. We recommend either:

     * SQLite on a fast, local disk. This is probably the fastest option and is perfectly adequate for many types of node. Note: if you are running Horizon, it will need to access stellar-core's database to ingest data. It is not compatible with SQLite. 
     * The newest version of PostgreSQL supported (a minimum version is listed in installation instructions but we usually test with newer versions as well).

         * Ideally running on the same physical machine as `stellar-core`
         * Ideally on instance storage, though RDS on modern instances is often reasonably low-latency.
         * Ideally communicating through a unix-domain socket. That is, with a connection string like `postgresql://dbname=core host=/var/run/postgresql`. This may require adjustments to PostgreSQL's `pg_hba.conf` file and/or `postgresql.conf`.

For illustration sake, the following table shows some latency numbers around ledger close-times measured on a test cluster running in AWS, with varying database configurations:

| Database connection type                                  | median  |    p75 |    p99 |   max |
|-----------------------------------------------------------|---------|--------|--------|-------|
| SQLite on NVMe instance storage                           |     1ms |    2ms |    2ms |  12ms |
| Local PostgreSQL on NVMe instance storage, Unix socket    |     3ms |    3ms |    3ms |  31ms |
| Local PostgreSQL on NVMe instance storage, TCP socket     |     3ms |    4ms |    4ms |  50ms |
| Local PostgreSQL on SSD EBS, Unix socket                  |     5ms |   20ms |   27ms | 169ms |
| Local PostgreSQL on SSD EBS, TCP socket                   |     4ms |   19ms |   54ms | 173ms |
| Remote PostgreSQL on RDS, TCP socket                      |    27ms |   87ms |  120ms | 170ms |

## Notes and advice from existing node operators

**TBD**
