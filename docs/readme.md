# Stellar-core Overview

Stellar is a decentralized, federated peer-to-peer network that allows people to
send payments in any currency anywhere in the world instantaneously, and with
minimal fee.

`Stellar-core` is the core component of this network. `Stellar-core` is a C++
implementation of the Stellar Consensus Protocol configured to construct a chain
of ledgers that are guaranteed to be in agreement across all the participating 
nodes at all times.

For more detail on the Stellar Consensus Protocol and how it establishes this
guarantee see [`src/scp/README.md`](../src/scp/readme.md). 

##Key Concepts

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
  own hash. (See [`src/xdr/Stellar-ledger.x`](../src/xdr/Stellar-ledger.x))

- **Transaction**: Making a payment, creating an offer and so forth. Anything
  that changes a ledger's entries is called a transaction.

- **Transaction set**: Set of transactions that are applied to a ledger to
  produce the next one in the chain.


`Stellar-core` maintains the content of the latest ledger and of the ledger
chain in a number of different representations in order to satisfy competing
performance needs.

 1. The latest ledger is stored in a postgresql or sqlite database in order to
    provide full "acid" safety and fast access to current state, notably to make
    it possible to determine efficiently whether a newly submitted operation is
    valid. (See the `load` and `store` functions in the subclasses of
    `EntryFrame`: `AccountFrame`, `TrustFrame`, and `OfferFrame` in
    [`src/ledger`](../src/ledger)).

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
    [`src/bucket/readme.md`](../src/bucket/readme.md)). While the hash computed 
	by the bucket list is functionally equivalent to a hash obtained 
	concatenating all the entries, it is not the same value since the bucket 
	list deduplicates changed entries incrementally.

 4. Finally, `stellar-core` can be configured to upload detailed historical
    records of all the transactions, including all or most of the ledgers'
    content, to persistent long-term storage. This record can be used to audit
    the full ledger chain's history, and is used to catch-up new nodes and nodes
    that have fallen far behind the rest of the network without imposing an
    undue burden on the nodes participating in the consensus protocol (See
    [`src/history/readme.md`](../src/history/readme.md)).


##Major Components

There are a few major components of the system. Each component has a dedicated
source directory and its own dedicated `readme.md`.


* **SCP** is our implementation of the Stellar Consensus Protocol (SCP). This
  component is fully abstracted from the rest of the system. It receives
  candidate black-box values and signals when these values have reached
  consensus by the network (called _externalizing_ a value) (See
  [`src/scp/readme.md`](../src/scp/readme.md)).

* **Herder** is responsible for interfacing between SCP and the rest of
  `stellar-core`. Herder provides SCP with concrete implementations of the
  methods SCP uses to communicate with peers, to compare values, to determine
  whether values contain valid signatures, and so forth. Herder often 
  accomplishes its tasks by delegating to other components 
  (See [`src/herder/readme.md`](../src/herder/readme.md)).

* **Overlay** connects to and keeps track of the peers this nodeis knows
  about and is connected to. It floods messages and fetches from peers the data
  that is needed to accomplish consensus (See 
  [`src/overlay/readme.md`](../src/overlay/readme.md)). All
  other data downloads are handled without imposing on the SCP-nodes, see 
  [`./architecture.md`](/docs/architecture.md).
  
* **Ledger** applies the transaction set that is externalized by SCP. It also
  forwards the externalization event to other components: it submits the changed
  ledger entries to the bucket list, triggers the publishing of history, and
  informs the overlay system to update its map of flooded messages. Ledger also
  triggers the history system's catching-up routine when it detects that this
  node has fallen behind of the rest of the network (See
  [`src/ledger/readme.md`](../src/ledger/readme.md)).

* **History** publishes transaction and ledger entries to off-site permanent
  storage for auditing, and as a source of catch-up data for other nodes. When
  this node falls behind, the history system fetches catch-up data and submits
  it to Ledger twice: first to verify its security, then to apply it (See
  [`src/history/readme.md`](../src/history/readme.md)).

* **BucketList** stores ledger entries on disk arranged for hashing and
  block-catch-up. BucketList coordinates the hashing and deduplicating of
  buckets by multiple background threads 
  (See [`src/buckets/readme.md`](../src/buckets/readme.md)).

* **Transactions** implements all the various transaction types (See
  [src/transactions/readme.md](../src/transactions/readme.md)).


## Supporting Code Directories

* **src/main** handles booting, loading of the configuration and of persistent 
  state flags. Launches the test suite if requested.

* **src/crypto** contains standard cryptographic routines, including random 
  number generation, hashing, and base-58 and hex encoding.

* **src/util** gathers assorted logging and utility routines.

* **src/lib** keeps various 3rd party libraries we use.

* **src/database** is a thin layer above the functionality provided by the
  database-access library `soci`.
  
* **src/process** is an asynchronous implementation of `system()`, for running
  subprocesses.

* **src/simulation** provides support for instantiating and exercising 
  in-process test networks.

* **src/xdr** contains to definition of the wire protocol in the [`XDR`
    (RFC4506)](https://tools.ietf.org/html/rfc4506.html) specification language.

* **src/generated** contains the wire protocol's C++ classes, generated from 
  the definitions in `src/xdr`.


## Additional Documentation

This directory contains the following additional documentation:

* [testnet.md](/docs/testnet.md) is short tutorial demonstrating how to 
  configure and run a short-lived, isolated test network.

* [architecture.md](/docs/architecture.md) describes how `stellar-core` is
  structured internally, how it is intended to be deployed and the collection of
  servers and services needed to get the full functionality and performance.

* [admin.md](/docs/admin.md) describes the configuration concerns and documents 
  the command line options.

