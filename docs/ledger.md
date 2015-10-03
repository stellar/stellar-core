---
title: Ledger
---

- **Ledger**: A ledger is the state of the distributed Stellar database at a
  particular point in time. It is composed of a set of _ledger entries_,
  including (1) accounts and their balances, (2) buy and sell offers, (3) and
  trust lines, as well as a _ledger header_ that records some additional meta
  data. Ledgers are linked together in a _ledger chain_. Each ledger has a
  sequence number that tells you where in the chain it falls.

- **Ledger chain**: This is an ever increasing list of ledgers. Each ledger
  points to the previous one thus forming a chain of history stretching back in
  time.

- **Ledger header**: The ledger's header contains meta data about the ledger,
  including the hash of the previous ledger (thus recording the chain) and its
  own hash. (See [`src/xdr/Stellar-ledger.x`](/src/xdr/Stellar-ledger.x))


`Stellar-core` maintains the content of the latest ledger and of the ledger
chain in a number of different representations in order to satisfy competing
performance needs.

 1. The latest ledger is stored in a postgresql or sqlite database in order to
    provide full "acid" safety and fast access to current state, notably to make
    it possible to determine efficiently whether a newly submitted operation is
    valid. (See the `load` and `store` functions in the subclasses of
    `EntryFrame`: `AccountFrame`, `TrustFrame`, and `OfferFrame` in
    [`src/ledger`](/src/ledger)).

 2. The ledger chain is represented in the the ledger headers as hashes linking
    each hedger to the previous one. The spine of the chain is lightweight data
    structure that can be scanned quickly to confirm that the current ledger is
    part of a trusted unbroken chain.

 3. In addition to being stored in the database, ledgers entries are stored on
    disk in a linearized format using a specific arrangement of
    exponentially-larger buckets, called the _bucket list_. This representation
    achieves two different goals. First, it makes it possible to compute the
    hashes of new ledgers incrementally and quickly, especially in the case
    where a minority of the accounts see the majority of the operations. Second,
    it participates in providing nodes that have fallen behind with the ledger
    data they need to catch-up with the current state of the chain. (See
    [`src/bucket/readme.md`](/src/bucket/readme.md)). While the hash computed
	by the bucket list is functionally equivalent to a hash obtained
	concatenating all the entries, it is not the same value since the bucket
	list deduplicates changed entries incrementally.

 4. Finally, `stellar-core` can be configured to upload detailed historical
    records of all the transactions, including all or most of the ledgers'
    content, to persistent long-term storage. This record can be used to audit
    the full ledger chain's history, and is used to catch-up new nodes and nodes
    that have fallen far behind the rest of the network without imposing an
    undue burden on the nodes participating in the consensus protocol (See
    [`src/history/readme.md`](/src/history/readme.md)).

