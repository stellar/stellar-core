---
title: Process-level architecture
---

Application owns a ledger-forming component, a p2p "overlay" component for
connecting to peers and flooding messages between peers, a set-synchronization
component for arriving at likely-in-sync candidate transaction sets, a
transaction processing component for applying a consensus transaction set to
the ledger, a crypto component for confirming signatures and hashing results,
and a database component for persisting ledger changes.
Two slightly-obscurely-named components are:

  - "bucketList", stored in the directory "bucket": the in-memory and on-disk
    linear history and ledger form that is hashed. A specific arrangement of
    concatenations-of-XDR. Organized around "temporal buckets". Entries tending
	to stay in buckets grouped by how frequently they change.
	see [`src/bucket/readme.md`](/src/bucket/readme.md)

  - SCP -- "Stellar Consensus Protocol", the component implementing the
    [consensus algorithm](https://www.stellar.org/papers/stellar-consensus-protocol.pdf).

Other details:

  - Single main thread doing async I/O and forming consensus; multiple
    worker threads doing computation (primarily memcpy, serialization,
    hashing). No multithreading on the core I/O or consensus logic.

  - No secondary internal "work queue" / scheduler, nor secondary internal
    packet transmit queues. Any async work is posted to either of the main
    or worker asio io_service queues. Any async transmits are posted as
    asio write callbacks that own their transmit buffers.

  - No secondary process-supervision process, no autonomous threads /
    complex shutdown requests. Can generally just destroy the application
    object (worker thread-joining is the only wait condition during
    shutdown).

  - Virtualized time, so that server can be cranked forwards at fast
    simulated time or have simulated time delays during testing. No
    real-time timeouts (except the one that synchronizes virtual and real
    time, in production).

  - Storage is split in two pieces, one bulk/cold Bucket-based store (history)
    kept in flat files, and one hot/indexed store (SQL DB). Both kept primarily
    _off_ the validator nodes.

  - No direct service of public HTTP requests. HTTP and websocket frontends
    are on separate public/frontend servers.

  - Sufficiently few globals (logging, CSPRNG) that one can run multiple
    application instances in-process and connect them together for loopback
    testing.

  - No use of boost. Use C++11 when possible, task-specific libraries
    when required.

  - No use of custom serialization format, nor embedding in protobufs. Uses
    single, standard XDR for canonical (hashed) format, history, and
    inter-node messaging.

  - No use of custom datatypes (No custom time epochs, currency codes, decimal
    floating point, etc.)


Network-level architecture
==========================

Validators are kept as simple as possible and offload as much responsibility as
they can to other parts of the system. In particular, validators do not store
or serve long-term history archives; they do not operate a transactional
(on disk) store for the "current state of the ledger"; they do not serve public
HTTP requests directly. These roles are offloaded to servers that are better
suited to these tasks, for which there are existing/better software stacks;
validators should have an "even" and predictable system-load profile. Validators
are also kept as stateless as possible keeping disk and memory constraints in
mind.

- Set of core validator nodes. Running stellar-core only. Tasked with:
  - Reaching consensus on a transaction set
  - Applying the tx set to their last ledger
  - Hashing current/recent/last-snapshot state
  - Sending SQL commands to connected DB to externalize changes
  - Writing history log to disk, running archival commands
  - Sending notification of changes to observation channel

- SQL DB nodes. One per validator (or one + failover, however we make an
  SQL server sufficiently safe, eg. RDS). Directly associated with that
  validator. Tasked with:
  - Maintaining ACID view of "current ledger" as txs are applied by validator.
  - Serving queries about state of current ledger to HTTP nodes.
  - Possibly storing recent-changes / hot-history while current
    cold-history block is being composed by validator (TBD).
  - Not necessarily being backed up; semi-transient (medium-term)
    store. Can be flushed and reloaded in bulk. Will be periodically, if
    validator is offline and possibly just as part of regular
    housekeeping. Long-term signed, canonical state is stored in bulk form
    in history archives.

- Set of public HTTP nodes. Not running stellar-core. Running
  apache/nginx/node/HTTP stack of choice. Flexible. Tasked with:
  - HTTP traffic from the outside world
  - Serving queries about ledger state from one of the SQL DBs
  - Accepting txs, performing sanity checks, submitting to the associated
    validator (possibly as XDR, or normalized json) as proposal, awaiting
    acceptance or rejection signal from observation channel, loading results
    from SQL DB
  - HTTP 300-redirecting history requests into appropriate history archive
    and/or synthesizing hot-history blocks from the SQL store (TBD).
  - Setting up and serving any additional materialized views /
    trigger-maintained tables inside SQL DB that are of interest to public
    service. Quite flexible here; no impact aside from SQL perf on core
    validators.

- History archives. Long term blob storage in S3/GCS/Azure. Tasked with:
  - Storing the consensus transaction log and set of ledger snapshots in
    canonical form (compressed XDR blocks, aim for many megabytes, but not
    gigabytes, per block).
  - Accepting new blocks from validators running archival commands, at
    frequency between minutes and hours. Effectively "continuous backup".
  - Serving history to validators that are new or out of sync.
  - Serving history to the general public who want to analyze it / back it up.
  - All access through HTTP, and/or REST command-line tools from services.
    These are not servers we necessarily run (though we can run extras).

- Observation channel for validators notifying public HTTP nodes of tx
  results. TBD. Simplest technique is to use the LISTEN/NOTIFY machinery
  built into postgres/libpq, though that commits us to postgres pretty
  firmly. If unacceptable, use an external message queue. This is just to
  trigger wakeups on public HTTP nodes awaiting tx results. Worst case /
  failure mode, they can timeout/poll. Messages are idempotent,
  content-free pings.

- (optional): Set of public validator nodes. Running stellar-core only. Tasked
with:
  - Listening to core validators and propagating their decisions blindly
    to anyone who asks.
  - Optionally feeding (some?) tx proposals submitted to them into core network.
  - Basically just a stellar-network load balancer / firewall for public access,
    for people who don't want to form trust relationships.
