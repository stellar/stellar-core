# Transactions
See [Concept documentation](https://www.stellar.org/developers/guides/concepts/transactions.html).

Anything that changes the ledger is called a _Transaction_.
Transactions have an arbitrary list of operations inside them.

See the "struct Transaction" definition in src/xdr/Stellar-transaction.x
for the protocol definition.
See the TransactionFrame class for the implementation.


*********************
## Fees
The network charges a fee for each transaction - right now the fee is just 
proportional to the number of operations inside it. See `TransactionFrame::getMinFee` for the actual implementation.

The base fee (multiplier) is decided during consensus; the desired base fee for 
each instance is defined in their configuration file.

## Sequence Number
Transactions follow a strict ordering rule when it comes to processing of 
transactions per account:
* Each transaction has a sequence number field.
* The transaction number has to be the next sequence number based off the one 
  stored in the source account when the transaction is applied.
* The next sequence number is obtained by incrementing the current sequence number by 1.
* The account entry is updated with the next sequence number of the transaction 
  when the transaction is applied.


Note that if several transactions are submitted as part of the same ledger, the 
rule is the same as if they were applied one after the other, which translates 
to their sequence numbers being continuous: if 3 transactions are submitted and 
the account is at sequence number 5, the transactions must have sequence 
numbers 6, 7 and 8.

## Validity of a transaction

A transaction is considered valid if and only if:

* __A__
 * the Source Account exists
 * the transaction is signed for the source account (see Signatures below)
 * the signatures together meet the "low" threshold
 * fee is less than maxFee
 * the sequence number follows the rules defined in the "Sequence Number" section
 * the current ledger sequence number is within the [minLedger, maxLedger]  range
and
* __B__
 * all operations are valid
 * all signatures attached to the transaction are used (by operations or by the 
   check in part __A__ )

Checking for the validity of a transaction is used by other modules to decide 
if a transaction should be forwarded to other peers or included in the next 
transaction set.

__Note__ that because transactions are not applied during validation (in this C++ 
implementation), there is a chance that a transaction invalidates another 
transaction; therefore this is really a best effort implementation specific.

## Applying a transaction

When SCP externalizes the transaction set to apply to the last closed ledger:
1. the Source accounts for all transactions are charged a fee
2. transactions are applied one by one, checking and updating the account's
   sequence number.

note that in earlier versions of the protocol (9 and below), the sequence
 number was updated at the same time than when the fee was charged.

To apply a transaction, it first checks for part __A__ of the validity check.

If any operation fails, the transaction is rolled back entirely but marked as 
such: the sequence number in the account is consumed, the fee is collected and 
the result set to "Failed".

## Result
When transactions are applied (success or not), the result is saved in the 
"txhistory" and "txfeehistory" tables in the database.

# Operations

Operations are individual commands that mutate the ledger.

Operations are executed "on behalf of" the source account specified in the 
transaction, unless there is an override defined for the operation.

See the "Multiple signature" section below for examples of how to use different 
source accounts in operations.

## Thresholds

Each operation falls under a specific threshold category: low, medium, high.
Thresholds define the level of priviledge an operation needs in order to succeed.

* Low Security:
  * AllowTrustTx
  * Used to allowing other signers to allow people to hold credit from this 
   account but not issue it.
* Medium Secruity:
  * All else
* High Security:
  * SetOptions for Signer and threshold
  * Used to change the Set of signers and the thresholds.

See the section on signatures below for more details.

## Validity of an operation

An operation is valid if:
* the outer transaction has enough signatures for the "source account" of the 
  operation to meet the threshold for that operation
* doCheckValid is true

Operations implement a "doCheckValid" method that is evaluated when trying to 
determine if a transaction is valid.

Checks made there are ones that would stay true regardless of the ledger state;
for example, are the parameters within the expected bounds?
It should not make checks that depend on the state of the ledger as the checks
are invalidated as other operations are being applied.

## Applying an operation

Operations implement a "doApply" method that implements the results of that 
operation.

When operations are applied they track changes to the ledger using SQL 
transactions and LedgerDelta objects, this makes rolling back changes simpler.

## Result

For each Operation, there is a matching Result type that gathers information on the key side effects 
of the operation or in the case of failure records why in structured form.

Results are queued in the txhistory table for other components to derive data:
historical module for uploading it for long term storage, but also for API 
servers to consume externally.
The txfeehistory table is additional meta data that tracks changes to the ledger
done before transactions are applied.

## List of operations
See `src/xdr/Stellar-transaction.x` for a detailed list of all operations and results.

## Implementation
For each operation type, there is a matching Frame class: for example, the Payment Operation has a PaymentFrame class associated with it.

The OperationFrame class defines the base contract that an operation frame must follow.

# Envelope and Signatures
Transactions must be signed before being submited to the network.

## Well formed signatures
A signature is a digital signature of the body of a transaction, generated with a private key.
Only some keys are authorized to sign transactions, see the "Signers" section below for more detail.

See the section on "Validity of a transaction" and "Validity of an operation" 
for other requirements that must be met by the transaction in terms of signatures.

## Signers
Accounts are identified with the public key of what is called the "master key".
Additional signers can be added to any account using the "SetOptions" operation.
When added, signers are authorized (in addition to the master key) to sign 
transactions for the source account. 

If the weight of the master key is ever updated to 0, the master key is considered to be an invalid
key and you cannot sign any transactions with it (even for operations with a threshold value of 0).
If there are other signers listed on the account, they can still continue to sign transactions.

"Signers" refers to the master key or to signers added later.

A signer is defined as the pair (public key, weight) - see the "Thresholds" 
section below for more detail.

Adding signers increases the reserve for the account.

## Thresholds

Thresholds are a property of an account entry that controls
* the weight 'w' of the master key (default: 1)
* the threshold to use for "low", "medium" and "high" (default: 0 for all)

Thresholds are also noted [w l m h].

The weight of a signature is the weight corresponding to the key that was used 
to sign the transaction.

A set of signatures meets a given level of privilege if the sum of weights of 
the signatures is greater or equal to the threshold for that level.

## Examples

### Operation Examples
1. Exchange without third party

  A wants to send B some credits X (Operation 1) in exchange for credits Y (Operation 2).

  A transaction is constructed:
  * source=_A_
  * Operation 1
    * source=_null_
    * Payment send X -> B 
  * Operation 2
    * source=_B_
    * Payment send Y -> A

   Signatures required:
  * Operation 1: requires signatures for Account A (the operation "inherits" 
    the source account from the transaction) to meet "medium" threshold
  * Operation 2: requires signatures for Account B to meet "medium" threshold
  * the transaction requires signatures for Account A to meet "low" threshold

  -> signing with the private keys for "A" and "B" is sufficient.

2. Worker nodes

   A gateway wants to divide the processing of their hot wallet between hosts 
   and keep track of sequence numbers locally.

   * Each host gets a private/key pair associated with them H1..Hn
   * H1...Hn are added as Signers to the hot wallet account "hotWallet", with 
     weight that gives them "medium" rights.
   * Then, when a node _i_ wants to submit a transaction to the network, it constructs the transaction:
      * source=_public key for Hi_
      * sequence number=_Hi sequence number_
      * Operation
        * source=_hotWallet_
        * Payment send X -> A
   * sign it with the private key Hi.

   The benefit of this scheme is that each host Hi can increment the sequence 
   number locally without having to deal with an external process submitting a 
   transaction on the same account (and invalidating some of the host's transactions).

3. Long lived transactions

   If the "exchange transaction" from example #1 can take an arbitrary long time
   and therefore block A's account as the transaction is constructed with a 
   specific transaction number, a scheme similar to #2 can be used.

   A would create a temporary account "Atemp", and add "A" as signer to the account "Atemp" with a weight of "low".

  A transaction is then constructed like this:
  * source=_Atemp_
  * sequence number=_Atemp seq num_
  * Operation 1
    * source=_A_
    * Payment send X -> B 
  * Operation 2
    * source=_B_
    * Payment send Y -> A

  The transaction would have to be signed by both A and B, but the sequence 
  number consumed will be from account Atemp.

  An additional operation "Operation 3" can be included to recover the XLM 
  balance from Atemp, "A" must be given "high" weight for this to work: 
  * Operation 3
    * source=_null_
    * Account Merge -> A

### Threshold Use Examples
1. Gateway keeps most of its funds in a cold account. The Gateway requires 
   authorization for people to hold its credit. It adds another signing key to 
   the cold account with a weight below Medium. It is now safe to use this 2nd 
   key on its server to authorize people to hold its credit. 
 - w:3, l:0, m:2, h:2     
 - other key:1
2. 3 people have a joint account. Any 1 of the 3 people can authorize a payment 
   on an account. All people have to agree to change signers on the account.
 - w:1, l:0, m:0, h:3
 - key2:1
 - key3:1
3. Company account requires 3 of 6 people to agree to any transaction from that 
   account.
 - w:0, l:3, m:3, h:3  (accountID key turned off, making it an invalid signing key)
 - keyN:1  (6 other keys all with weight of 1)
4. Expense account. 1 person fully controls the account and 2 employees can 
   authorize transactions from this account. If one of the employees leaves the 
   controller can remove their signing key.
 - w:3 l:0, m:0, h:3
 - key2:1
 - key3:1
5. Someone wants to issues a custom currency and wants to ensure no more will 
   ever be created. They make a source account and issue the maximum amount of 
   currency they want to a holding account. Now they set the weight of the master
   key of the source account to 0, which makes it an invalid signing key.
 - w:0 l:0 m:0 h:0

