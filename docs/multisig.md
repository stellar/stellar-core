# Multisig
Stellar accounts can be made multisig simply by adding additional signers to the account.

Every account entry has a list of signers and their corresponding weights.

There are 3 categories of transactions. Each has a separate threshold that can be set and stored in the account entry.

* Low:
 * AllowTrustTx
* Medium:
 * All else
* High:
 * SetOptions for Signer and threshold

Accounts can have arbitraily many signers. Each additional signer increases the ownerCount and therfore the min balance the account must maintain.

Transactions can have multiple signatures attached. If a transaction requires multiple signatures the txblob must be passed around out of band.
  
###Example use cases
1. Gateway keeps most of its funds in a cold account. The Gateway requires authorization for people to hold its credit. It adds another signing key to the cold account with a weight below Medium. It is now safe to use this 2nd key on its server to authorize people to hold its credit.
2. 3 people have a joint account. Any 1 of the 3 people can authorize a payment on an account. All people have to agree to change signers on the account.
3. Company account requires 3 of 6 people to agree to any transaction from that account.
4. Expense account. 1 person fully controls the account and 2 employees can authorize transactions from this account. If one of the employees leaves the controller can remove their signing key.
5. Escrow. A wants to send to B.  A deposits funds into an escrow account that takes a signature of A and the agent to authorize a transaction. A provides a signed transaction moving funds from the escrow account to B. When the agent hears from B it can either return the funds to A or sign the transaction sending to B.
6. Someone wants to issues a custom currency and wants to ensure no more will ever be created. They make a source account and issue the maximum amount of currency they want to a holding account. Now they set the weight of the source account signing key below the Medium threshold.

