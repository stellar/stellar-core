#What is a ledger?
A ledger represents the state of the Stellar universe at a given point in time.
The first ledger in history is called the genesis ledger.

Every SCP round, consensus decides on which transaction set to apply to the
last closed ledger; when the new set is applied, a new "last closed ledger"
is defined.

Each ledger is cryptographically linked to a unique previous ledger, creating
a historical chain that goes back to the genesis ledger. 
We define the sequence number of a ledger recursively:
* genesis ledger has sequence number 1
* a ledger directly derived from a ledger with sequence number n, has sequence
    number n+1

#Data structure organization

The way the ledger data is organized is via a container called a "LedgerHeader".
This container has references to the actual data within the ledger as well as
a reference to the previous ledger.
References here are cryptographic hashes of the content being referenced, which
behaves just like pointers in typical data structures but with added
security garantees.

See the protocol file for the object definitions.
src/xdr/Stellar-ledger.x

One can think of the historical chain as a linked list of LedgerHeader:

[Genesis] <---- [LedgerHeader_1] <----- ... <---- [LedgerHeader_n]

##LedgerHeader organization
Each LedgerHeader has many references described below.

Some key properties are directly inside the LedgerHeader such as the number of
coins present at this given time.

##Back references
The way a ledger header refers to a previous ledger is actually done with
alternate validation in mind.
###The hash of the previous ledger header
It is there to link the sequence of ledgers as previously described.

###The hash of the transaction set from SCP
This direclty links consensus messages to the chain as what consensus signs
includes the transaction set.

The transaction set is what was used as input to the transaction engine to
transition a ledger to the next.

###The hash of the list of result for each transaction
This data is not stricly speaking necessary for validating the chain, but
makes it easier for entities to validate the result of a given transaction
without having to replay and validate the entire ledger state.

##Bucket list hash
This is a reference to a multi level tree like structure described in more
detail in src/bucket.
The leaf elements are what we call "Ledger Entries", this is the bulk of
the data contained in a ledger.

Ledger entries are specified in 
src/xdr/Stellar-ledger-entries.x

###AccountEntry
This entry represents an account. In Stellar, everything is centered around
accounts: transactions are performed by an account.

Accounts control the access rights to balances.

The other entries are "add-ons" to the main account entry; with every new entry
attached to the account, the minimum balance in XLM goes up for the
account (also known as reserve). See LedgerManager::getMinBalance for more detail

###TrustLineEntry
Trust lines are lines of credit the account has with a specific currency
from an issuer.

It defines the rules around the use of this currency.
Rules can be defined by the user (balance limit to limit risk), or by
the issuer (authorized flag for example).

###OfferEntry
Offers are entries in the Offer Book that an account creates.
Offers are a way to automate simple trading off the Stellar network.

#Source code organization

Frame classes are wrapper classes for the related (generated) classes
from the protocol. For example, AccountFrame adds methods to the
AccountEntry container.

##LedgerManager
This is the ledger module used to manage the current ledger:
* during normal operation, SCP calls the main "externalizeValue" method
    when consensus has been reached.
* when out of sync, the history module calls catch up related methods.

LedgerManager gives other modules ways to query ledger information, like
current ledger sequence number, or last closed ledger), and also to
close the current ledger given a context that includes "close time" and
"transaction set".

See the "Closing ledger" section for more detail on what happens when
closing a ledger.

##LedgerDelta
This class represents all side effects that occured while applying transactions.
It keeps tracks of creation, modification and deletion of Ledger Entries as
well as changes to the ledgerHeader.
LedgerDelta is a nestable structure, which allows fine grain control of which
subset of changes to include or not in the final set of changes that will be
commited to the ledger.

For more detail see the "Closing a ledger" section.

#Closing a ledger

When closing a ledger, the engine needs to apply the consensus transaction set
to the last closed ledger to produce a new closed ledger.

The method that does this is "LedgerManagerImpl::closeLedger".

First the transaction set is reordered in apply order:
during consensus, the transaction set was sorted by hash to keep things simple,
but when it comes to actually applying them, they need to be sorted such that
transactions for a given account are applied in sequence number order and also
randomized enough so that it becomes unfeasible to submit a transaction and
garantee that it will be executed before or after another transaction in the set.

See TxSetFrame::sortForApply for more detail.

Once the list of transactions to apply is computed, each transaction is
applied to the ledger.

See src/transactions/ for more detail on how transactions are applied.

After applying each transaction its result is stored in the transaction history
table (see Historical data) and side effects (captured in LedgerDelta) are saved.

After all transaction have been applied, all changes are ready to be commited to
the current state of the database via SQL commit and to the overall LedgerDelta
for the entire Ledger close is fed to the BucketManager (see BucketManager).

At this point the module notifies other modules that a ledger was closed:
* herder so that it can trigger a new SCP round (see src/herder )
* overlay to reset its state and prepare accumulating changes for the next
    ledger (see src/overlay )
* historical subsystem, so that it can publish the new ledger/transaction set
    for long term storage (see src/history )

#Storage

The ledger state is persisted in two ways.

##SQL backed data
We use SQL tables to store data. Each *Frame class is responsible for storing
and retrieving data in its respective table.

For more detail on the SQL implementation, see src/Database/

###Hot Ledger Data

The SQL tables for Ledger Entries represents the state of the current ledger:
ie, if an account is modified in some way, the "Accounts" table will have the change.

###Historical data
Some tables are used as queues to other subsystems: 

LedgerHeader contains the ledger headers that were produced by the "closeLedger"
method in LedgerManager.

TxHistory contains the record of all transactions applied to all ledgers that
were closed. See src/transactions/TransactionFrame.cpp for more detail


##BucketManager
The final LedgerDelta generated by closing the ledger is fed into the
BucketManager to add it to the "L0" bucket.
The resulting set is used to compute the hash of the entire set of
Ledger Entries.

See src/bucket for more detail.

