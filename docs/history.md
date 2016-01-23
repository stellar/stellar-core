---
title: History
---

Stellar Core, or stellar-core, separates data into "current" and "historical."

Current data is the subject of peer-to-peer messages--consensus is only concerned with the present,
not the past. Current data _resides_ in a local SQL database paired with each stellar-core
process. This database is consulted and updated "live" in an ACID fashion as stellar-core applies
each [transaction](transaction.md) set for which consensus was reached and forms each new [ledger](ledger.md).

Unlike many similar systems, stellar-core does _not_ need to consult history in order to apply a
single transaction or set of transactions. Only "current" data--the state of the current ledger
and the hash of the previous one--is required to apply transactions. "Historical" data exists for
peers to catch up to one another, as well as for record auditing and interoperability with other
programs.

Historical data resides in _history archives_. A stellar-core process should almost always have
access to a history archive. While it is possible to run a stellar-core process without any
associated history archives, other peers will not be able to catch up with it, and it will not be
able to catch up with other peers, so it will likely be a very incomplete configuration. For
normal operations, a stellar-core process should always be configured with one or more history
archives.

For history archives to be effective, they should be configured in such a way that each validator
writes to one or more archives, which are in turn readable by one or more other peers. A common
configuration is for each peer in a group to have a single history archive that it knows how to
write to, while also knowing how to read from all archives in the group.


## History archives

Many different facilities or services can be used as history archives; stellar-core only needs to be
supplied with a way to "get" and "put" files to and from the archive. For example, history archives
may be FTP or SFTP servers, filesystem directories shared between stellar-core processes, AWS S3,
Google Cloud Storage, Azure Blob storage or similar commodity object storage services.

History archives are defined in a very lightweight fashion, in stellar-core's configuration file, by
providing a pair of `get` and `put` command templates. stellar-core will run the provided command
template, with its own file names substituted for placeholders in the template, in order to get files
from, and put files into, a given history archive. This interface is meant to support simple
commands like `curl`, `wget`, `aws`, `gcutil`, `s3cmd`, `cp`, `scp`, `ftp` or similar. Several
examples are provided in the example configuration files.


## Serialization to XDR and gzip

stellar-core leans heavily on the XDR data format. This is an old, stable, standardized
serialization format, defined in RFC 4506 and used for several standard unix and internet protocols
and formats.

XDR is used for 3 related but different tasks, the first 2 of which are discussed elsewhere:

  * Exchanging peer-to-peer network protocol messages and achieving consensus.
  * Cryptographically hashing ledger entries, buckets, transactions, and similar values.
  * Storing and retrieving history (discussed in this document).

When storing XDR files to history archives, stellar-core first applies gzip (RFC 1952) compression
to the files. The resulting `.xdr.gz` files can be concatenated, accessed in streaming fashion, or
decompressed to `.xdr` files and dumped as plain text by stellar-core.


## Checkpointing

During normal operation, a validating stellar-core server will save a "checkpoint" of its recent
operations to XDR files, compress them, and publish them to all of its configured writable history
archives once every 64 ledgers (about once every 5 minutes).

History checkpoints include the set of buckets that have changed since the last checkpoint (see
[description of buckets](ledger.md)) as well as the ledger headers, transaction sets, results of
transactions, and a small amount of indexing metadata stored in JSON files. This permits a variety
of fine-grained auditing, transaction replay, or direct catchup.

Checkpointing happens asynchronously based on snapshot read isolation in the SQL database and
immutable copies of buckets; it does not interrupt or delay further rounds of consensus, even if the
history archive is temporarily unavailable or slow. If a pending checkpoint publication fails too
many times, it will be discarded. In theory, every validating node that is in consensus should
publish identical checkpoints (aside from server-identification metadata). Thus, so long as _some_
history archive in a group receives a copy of a checkpoint, the files of the checkpoint can be
safely copied to any other history archive that is missing them.


## Catching up

When stellar-core finds that it is out of sync with its peers--either because it is joining
a network for the first time, or because it crashed or was disconnected for some reason--it
contacts a history archive and attempts to find published history records from which to "catch up."
This is the first and most essential use of history archives: they are how peers catch up with
one another.

This bears repeating: peers **never** send historical data to one another directly, and they
**must** share access to a common history archive if they're ever to successfully catch up with one
another when out of sync. If you run a stellar-core server without configuring history archives, it
will never synchronize with its peers (unless they all start at the same time).

The peer-to-peer protocol among stellar-core peers deals only with the current ledger's transactions
and consensus rounds. History is sent one-way from active peers to history archives, and is
retrieved one-way by new peers from history archives. Aside from establishing which value to
catch up to, peers do _not_ provide one another with the data to catch up when out of sync.

Catching up is based on buffering live network traffic in memory while awaiting a checkpoint that
overlaps with the buffered traffic. Usually any catchup operation must wait, on average, half the
duration of a checkpoint window (2.5 minutes) before enough material is buffered and published to
successfully catch up. The catchup time window varies depending on the responsiveness and
reachability of the history archive selected, but in general catchup will retry until it finds
enough history material to succeed.


## Auditing and interoperability

One of the factors motivating the stellar-core history design was to permit other programs and 3rd
parties transparent, easy, and unbiased access to the ledger and transaction history, without having
to "go through" the stellar-core program or protocol. Any program that can fetch data from a history
archive and deserialize XDR can read the complete history; there is no need to speak the
stellar-core peer-to-peer protocol or interact with any stellar-core peers.

With the exception of a single "most recent checkpoint" metadata file, all files written to a
history archive are written _once_ and never modified. Bucket files are named by hash, but
transaction sets, ledger headers, and checkpoint metadata (including the hashes of buckets) are named
sequentially. Anyone wishing to audit or reconstruct the activity of stellar-core by monitoring a
history archive can simply poll the archive and consume new files as they arrive.

All XDR encoding and decoding in stellar-core is done by code generated automatically from the
associated [XDR schemas](/src/xdr); any other compliant XDR code generator should produce a
deserializer that can read and write the same history. The XDR code generator used in stellar-core
is developed independently, but [included in the source tree as a submodule](../lib/xdrpp).


## Additional design considerations

In addition to the considerations of interoperability and software flexibility presented above, a
few additional, less obvious motives are at work in the design of the history system in stellar-core.
A few reasons that the extra effort of configuring independent history archives is, in our judgment, worth its slight awkwardness:

  - Configuring independent history archives helps ensure valid backups get made. It is very easy to build a backup system that is not run
    frequently enough, only run in emergencies, or never run at all. Such systems tend to accumulate
    bugs or misconfiguration that can render them useless in production. By forcing normal operation
    to use the _same code path_ that is making continuous, long-term flat-file backups, we help
    ensure the backup code _works_, and is being run on a regular schedule.

  - This design reduces the risk of lost peers. stellar-core peers are comparatively ephemeral: new ones can
    be brought online relatively quickly (only downloading missing buckets) and the state stored on
    a given peer is likely only one checkpoint, or 5 minutes, worth of unique data (the rest has
    been archived). While stellar-core is designed to run as a highly fault-tolerant replicated
    system in the first place, the less damage suffered by losing a single replica, the better.

  - It is fast, flexible and cheap. Copying bytes sequentially from flat files is the case that all
    operating systems, file service providers, and networking systems are optimized for. Anything
    more interactive or involving more round trips or random seeking would be slower. The service
    also parallelizes almost perfectly and is provided by a wide variety of highly competitive
    vendors.

  - Finally, configuring independent history archives enforces a separation between (time-sensitive) consensus and (time-insensitive) history
    traffic, which is good for isolating and managing system load. New peers coming online may need
    significant amounts of data to catch up; if they requested this data from existing peers, they would put
    immediate load on those peers and could interfere with those peers performing the delicate and
    time-sensitive work of acquiring consensus and forming new ledgers. By performing catchup
    against independent history archives, the work can occur in parallel and use entirely
    separate resources (network, CPU, disk).

## Detailed structure of history archives

Each history archive contains a number of directories, each containing either `.json` files (JSON
format, RFC 7159) or `.xdr.gz` files (Gzipped XDR format, RFCs 1952 and 4506).

### History Archive State (HAS) files

The JSON files in a history archive all have a format called a "History Archive State" or "HAS":
they are a JSON serialization of the data structure called
[HistoryArchiveState](/src/history/HistoryArchive.h) and contain the following fields:

  - `version`: number identifying the file format version
  - `server`: an optional debugging string identifying the software that wrote the file
  - `currentLedger`: a number denoting the ledger this file describes the state of
  - `currentBuckets`: an array containing an encoding of the [bucket list](/src/bucket/BucketList.h) for this ledger

The `currentBuckets` array contains one object for each level in the bucket list. The objects in the
array correspond to "levels" in the bucket list; any field in the bucket list which is said to denote
"a bucket" is a string that holds the hex-encoded SHA256 hash of the bucket. The level objects of
the bucket list contain the following fields:

  - `curr`: the "current" bucket for the level
  - `snap`: the "snap" bucket for the level
  - `next`: the "next" future-bucket for the level, an object with the following fields:
    - `state`: a number denoting the state of the future bucket. If `state == 0` then
    no other fields exist. Otherwise some fields may exist:
    - `output` (if `state == 1`): a fully-merged bucket
    - `curr` (if `state == 2`): the `curr` bucket used as input to a merge
    - `snap` (if `state == 2`): the `snap` bucket used as input to a merge
    - `shadow` (if `state == 2`): an array of buckets used as input to a merge

The organization of the bucket list is somewhat subtle: it is an 11-entry array of
temporally-stratified, exponentially-growing sequences of ledger entries, each of which comprises a
subset of ledger entries last-changed a given amount of time in the past. The detailed structure is
explained in [the BucketList.h header](/src/bucket/BucketList.h). It suffices here to say that the
higher-numbered levels in the bucket list denote larger buckets (with more ledger entries) that
change more slowly; whereas the lower-numbered levels denote smaller buckets (with fewer ledger
entries) that change more quickly. Reconstructing the state of the ledger described by a given
bucket list involves applying the buckets of ledger entries in reverse order, from higher to lower
levels.

### Root HAS

Every history archive has a "root" HAS which can be found at a fixed location within the archive:
`.well-known/stellar-history.json`. This is kept in the RFC 5785 `.well-known` directory, and is
intended as a "starting place" for any client reading from a history archive. It is a duplicate copy
of the most recent HAS file written to the archive. Reading the root HAS file gives enough
information to navigate the rest of the archive. The root HAS file is also the _last_ file written
to the history archive when any update is made, so represents an atomic "commit point" to changes to
the archive.

### Other files

Aside from the root HAS file, all other files in the history archive are named by one of two logical
naming schemes:

  - **By ledger number**: these files have the form `category/ww/xx/yy/category-wwxxyyzz.xdr.gz`, where:
    - `category` describes the content of the file (`history`, `ledger`, `transactions`, `results` or `scp`)
    - `0xwwxxyyzz` is the 32bit ledger sequence number for the checkpoint at which the file was written,
    expressed as an 8 hex digit, lower-case ASCII string.
    - `ext` is a file extension, either `.json` or `.xdr.gz`

  - **By hash**: these files have the form
    `bucket/pp/qq/rr/bucket-ppqqrrssssssssssssssssssssssssssssssssssssssssssssssssssssssssss.xdr.gz`
    where `ppqqrrssssssssssssssssssssssssssssssssssssssssssssssssssssssssss` is the 256-bit SHA256
    hash value of the contents of the bucket, expressed as a 64 hex digit, lower-case ASCII
    string. Only bucket files are stored in this form.

Checkpoints are made once every 64 ledgers, at ledger sequence numbers that are one-less-than a multiple of
64. For example, HAS files are written at checkpoints for the following ledger sequence numbers and paths:

   - ledger `0x0000003f`, stored in `history/00/00/00/history-0000003f.json`
   - ledger `0x0000007f`, stored in `history/00/00/00/history-0000007f.json`
   - ledger `0x000000bf`, stored in `history/00/00/00/history-000000bf.json`
   - ledger `0x000000ff`, stored in `history/00/00/00/history-000000ff.json`
   - ledger `0x0000013f`, stored in `history/00/00/01/history-0000013f.json`
   - ledger `0x0000017f`, stored in `history/00/00/01/history-0000017f.json`
   - ledger `0x000001bf`, stored in `history/00/00/01/history-000001bf.json`
   - ledger `0x000001ff`, stored in `history/00/00/01/history-000001ff.json`
   - etc.

These HAS files in turn contain all the hash-based names of buckets associated with each checkpoint.
Every other file in the archive is, like the HAS file, named by ledger number.


### Individual file contents

In total, each checkpoint number `0xwwxxyyzz` consists of the following files:

  - One HAS file, named by ledger number as `history/ww/xx/yy/history-wwxxyyzz.json`, as described
    above: a JSON file listing the individual buckets associated with the checkpoint.

  - Zero or more bucket files, named by hash as
    `bucket/pp/qq/rr/bucket-ppqqrrssssssssssssssssssssssssssssssssssssssssssssssssssssssssss.xdr.gz`.
    One such file should exist for every bucket mentioned in any of the bucket list fields in the
    checkpoint's HAS file, `history/ww/xx/yy/history-wwxxyyzz.json`. Bucket files are added as
    needed to the history archive; generally each checkpoint will involve changing one or more
    buckets but only those buckets that change _between checkpoints_ are uploaded with the
    checkpoint, the rest are assumed to already exist in the archive. The HAS file and the buckets,
    together, are sufficient to reconstruct the _state_ of the ledger at the checkpoint; but not
    enough to connect that state to the historical "trust chain" of the current network.

  - One ledger-headers file, named by ledger number as `ledger/ww/xx/yy/ledger-wwxxyyzz.xdr.gz`. The
    file contains a sequence of XDR structures of type
    [`LedgerHeaderHistoryEntry`](/src/xdr/Stellar-ledger.x), one per ledger in the checkpoint (so
    there should be 64 such structures in all checkpoints except the first, which has 63
    headers). These header structures are fixed-size and small, and are sufficient to establish the
    "trust chain" of linked cryptographic hashes between the present state of the network and
    previous, historical states.

  - One transactions file, named by ledger number as
    `transactions/ww/xx/yy/transactions-wwxxyyzz.xdr.gz`. The file contains a sequence of XDR
    structures of the type [`TransactionHistoryEntry`](/src/xdr/Stellar-ledger.x), with zero-or-more
    structures per ledger; it is the concatenation of all the transactions applied in all the
    ledgers of a given checkpoint. Each `TransactionHistoryEntry` structure indicates the ledger it
    was a part of, and there may be dozens, hundreds, even thousands of such structures per
    ledger. These files are potentially large, and should only be downloaded if _replaying_ history
    (rather than simply reconstructing a point-in-time) or verifying individual transactions.

  - One results file, named by ledger number as `results/ww/xx/yy/results-wwxxyyzz.xdr.gz`. The file
    contains a sequence of XDR structures of the type
    [`TransactionHistoryResultEntry`](/src/xdr/Stellar-ledger.x), with zero-or-more structures per
    ledger. The file is similar to the transactions file, in that there is one entry per transaction
    applied to a ledger in the checkpoint; but this file stores the _results_ of applying each
    transaction. These files allow reconstruction the history of changes to the ledger without
    actually running the `stellar-core` transaction-apply logic.

  - (Optionally) one SCP file, named by ledger number as `scp/ww/xx/yy/scp-wwxxyyzz.xdr.gz`. The file
    contains a sequence of XDR structures of the type
    [`SCPHistoryEntry`](/src/xdr/Stellar-ledger.x), with zero-or-more structures per ledger. The
    file records the sequence of nomination and ballot protocol messages exchanged during consensus
    for each ledger. It is primarily of interest when debugging, or when analyzing
    trust-relationships and protocol behavior of SCP. It is not required for reconstructing the
    ledger state or interpreting the transactions.

