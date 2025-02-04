# Buckets and the BucketList

Ledgers contain a (large) logical set of ledger entries, and store this set in a
local SQL [database](../database) in order to query the set and apply
transactions to it.

However, for two operations the "large logical set" of entries is inconvenient
and/or intractable in its SQL storage form:

- Efficiently calculating a cryptographic hash of the entire set after each
  change to it.

- Efficiently transmitting a minimal "delta" of changes to the set when a peer
  is out of sync with the current ledger state and needs to "catch up".

In order to support these two operations, the ledger entries are *redundantly*
stored in a secondary structure called the [BucketList](BucketList.h), which is
composed of a fixed number of "levels", each of which contains two
[Buckets](Bucket.h). Each level's Buckets are a multiple of the size of the
Buckets in the previous level, and Buckets store entries based on their age:
more-recently-modified entries reside in smaller, lower-level buckets.

The cumulative hash of the bucket levels creates a single "BucketList hash",
which is stored in the [ledger header](../xdr/Stellar-ledger.x) in order to
unambiguously denote the set of entries that exist at each ledger-close.

The individual buckets that compose each level are checkpointed to history
storage by the [history module](../history). The difference from the current bucket list (a subset
of the buckets) is retrieved from history and applied in order to perform "fast" catchup.

# BucketListDB Index

Previously, the state of the ledger is redundantly copied in two places: the local SQL database
and the BucketList. The BucketList is used to quickly compute the hash for a ledger, while the
database is used for key-value lookup. This disjoint setup leads to write and disk amplification.
Additionally, SQL is not the most efficient database for our read/write patterns. The database
receives a single large, atomic write whenever a ledger closes and many
reads during consensus. There is never a write during a read, so all the reads occur on an
effectively
immutable database. This write once read many times paradigm is suited better for a LSM structure
(the bucket list) than SQL DBs. In particular, Postgres is ACID compliant and supports
transactions that can be rolled back. This introduces significant, unnecessary overhead.
Since our access pattern never has conflicting reads and writes, ACID compliance is not required.
Additionally, the current core implementation never rolls back a SQL transaction. Finally,
all reads occur on an immutable database, opening the door for parallelism.
However, MySQL and Postgres do not treat the DB as immutable during reads
and cannot take advantage of this parallelism.

By performing key-value lookup directly on the BucketList, we can remove ACID/rollback overhead,
take advantage of parallelism, and optimize for our specific access patterns significantly better
than in a SQL DB.

## Design Overview

Due to the multi-level structure of the BucketList, it may be necessary to search all buckets
to find the target `LedgerEntry`. To avoid unnecessary disk IO, the `BucketIndex` maps
`LedgerKey`'s to the `LedgerEntry`'s corresponding offset in the bucket file. Each `Bucket` has a
corresponding `BucketIndex` kept in memory allowing minimal disk IO for each load.

Due to the large number of `LedgerKey`'s, it is not possible to map each entry in a
`Bucket` individually. Because of this, there are two types of indexes, the
`IndividualIndex` and the `RangeIndex`. The `IndividualIndex` maps individual `LedgerKey`'s to a
file offset as previously described. The `RangeIndex` maps a range of
`LedgerKey`'s to a given page in the file. Because the `LedgerEntry`'s in each
bucket file on disk are sorted, the `RangeIndex` can provide the page where a given
entry may exist. However, the `RangeIndex` cannot determine if the given entry exists
in the bucket or not. Especially in smaller buckets where the range of each page tends to
be large, there is a high rate of "false positives" where each element in the given page
must be iterated through in order to determine if the target entry exists in the bucket.

To avoid additional disk reads from these "false positives", the `RangeIndex` also uses a
bloom filter to determine if a given entry exists in the `Bucket` before doing a disk read
and iterating through the corresponding page. The bloom filter is probabilistic and still
has a false positive rate, but it is low (1/1000).

Even with the bloom filter, the `RangeIndex` must still iterate through each entry in a
page to find the target entry, making the `IndividualIndex` significantly faster. For
small buckets, the `IndividualIndex` is used, while larger buckets use the `RangeIndex`
for smaller memory overhead.

## Configuration Options

Because the `BucketIndex`'s must be in memory, there is a tradeoff between BucketList
lookup speed and memory overhead. The following configuration flags control these options:

- `BUCKETLIST_DB_INDEX_PAGE_SIZE_EXPONENT`
  - Page size used for `RangeIndex`, where `pageSize ==
    2^BUCKETLIST_DB_INDEX_PAGE_SIZE_EXPONENT`.
    Larger values slow down lookup speed but
    decrease memory usage.
- `BUCKETLIST_DB_INDEX_CUTOFF`
  - Bucket file size, in MB, that determines wether the Bucket is cached in memory or not.
    Default value is 250 MB, which indexes the first ~5 levels with the `IndividualIndex`.
    Larger values speed up lookups but increase memory usage.
- `BUCKETLIST_DB_PERSIST_INDEX`
  - When set to true, BucketListDB indexes are saved to disk to avoid reindexing
    on startup. Defaults to true, should only be set to false for testing purposes.
    Validators do not currently support persisted indexes. If NODE_IS_VALIDATOR=true,
    this value is ignored and indexes are never persisted.
