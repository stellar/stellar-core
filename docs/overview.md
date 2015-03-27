# Stellar-core

Stellar-core is a C++ implementation of the Stellar protocol. (see stellar_overview.md)

The goal is to be able to scale to 500M accounts and 2000 transactions/second on reasonable hardware.

There are a few major components of the system:

##SCP
This is our implementation of the SCP algorithm.
see http://www.scs.stanford.edu/~dm/noindex/scp.pdf
It has no knowledge of the rest of the system. 

##Herder
This is responsible for interfacing between SCP and the rest of stellar-core. It determines if SCP ballot values are valid or not.

##Overlay
This is the connection layer. It handles things like: 
- Keeping track of what other peers you are connected to.
- Flooding messages that need to be flooded to the network.
- Fetching things like transaction and quorum sets from the network.
- Trying to keep you connected to the number of peers set in the .cfg 

##Ledger
Handles applying the transaction set that is externalized by SCP. Hands off the resulting changed ledger entries to the BucketList.

##BucketList
Ledger entries arranged for hashing

##Transactions
The implementaions of all the various transaction types. (see transaction.md)

##crypto

##util
Logging and whatnot

##lib
various 3rd party libaries we use


##Concepts
- **Ledger**: This is the state of the world at a particular point in time. Ledgers are linked together in a `Ledger Chain`. Each ledger has a sequence number that tells you where in the chain it falls. A ledger is composed of a set of `ledger entries` and a `ledger header`.
- **Ledger chain**: This is an ever increasing list of `ledgers`. Each `ledger` points to the previous one thus forming a chain of history stretching back in time.
- **Ledger header**: (/src/xdr/Stellar-ledger.x) Meta information about a particular `ledger`.
- **Ledger entry**: One piece of data that is stored in the `ledger`. Can be thought of like a record in a DB. Examples are Accounts,Offers,TrustLines.
- **Bucket**: Contains a set of ledger entries. see /src/bucket/BucketList.h for more details.
- **Bucket List**: List of buckets. The buckets are hashed to produce the ledger hash. see /src/bucket/BucketList.h for more details.
- **Transaction**: Anything that changes the ledger entries is called a transaction. 
- **Transaction set**: Set of transactions that are applied to a ledger to produce the next one in the chain. 

