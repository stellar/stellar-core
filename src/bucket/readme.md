# Buckets and the BucketList

Ledgers contain a (large) logical set of ledger entries, and store this set in a
local SQL [database](../database) in order to query the set and apply
transactions to it.

However, for two operations the "large logical set" of entries is inconvenient
and/or intractable in its SQL storge form:

  - Efficiently calculating a cryptographic hash of the entire set, after each
    change to it.

  - Efficiently transmitting a minimal "delta" of changes to the set, when a peer
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
storage by the [history module](../history), and a subset of them -- the
difference from the current bucket list -- is retrieved from history and applied
in order to perform "fast" catchup.
