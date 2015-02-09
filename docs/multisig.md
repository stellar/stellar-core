# Multisig
Stellar accounts can be made multisig simply by adding additional signers to the account.

Every Account has a list of signers and their corresponding weights. The public key of the account is always a signer with a weight specified in the first byte of the threshold field in the Account Entry. (see ledger.md)

There are 3 categories of transactions. Each has a separate threshold that can be set and stored in the account entry.

* Low Security:
 * AllowTrustTx
 * allowing other keys to allow people to hold your credit but not issue it
* Medium Secruity:
 * All else
* High Security:
 * SetOptions for Signer and threshold
 * Change the Set of signers and the thresholds

Accounts can have arbitraily many signers. Each additional signer increases the ownerCount and therfore the min balance the account must maintain.

Transactions can have multiple signatures attached. If a transaction requires multiple signatures the txblob must be passed around out of band.
  
###Example use cases
1. Gateway keeps most of its funds in a cold account. The Gateway requires authorization for people to hold its credit. It adds another signing key to the cold account with a weight below Medium. It is now safe to use this 2nd key on its server to authorize people to hold its credit. 
 - w:3, l:0, m:2, h:2     
 - other key:1
2. 3 people have a joint account. Any 1 of the 3 people can authorize a payment on an account. All people have to agree to change signers on the account.
 - w:1, l:0, m:0, h:3
 - key2:1
 - key3:1
3. Company account requires 3 of 6 people to agree to any transaction from that account.
 - w:0, l:3, m:3, h:3  (accountID key turned off)
 - keyN:1  (6 other keys all with weight of 1)
4. Expense account. 1 person fully controls the account and 2 employees can authorize transactions from this account. If one of the employees leaves the controller can remove their signing key.
 - w:3 l:0, m:0, h:3
 - key2:1
 - key3:1
5. Escrow. A wants to send to B.  A deposits funds into an escrow account that takes a signature of A and the agent to authorize a transaction. A provides a signed transaction moving funds from the escrow account to B. When the agent hears from B it can either return the funds to A or sign the transaction sending to B. *Note: this actually doesn't work because of the seq numbers* 
6. Someone wants to issues a custom currency and wants to ensure no more will ever be created. They make a source account and issue the maximum amount of currency they want to a holding account. Now they set the weight of the source account signing key below the Medium threshold.
 - w:0 l:0 m:0 h:0
