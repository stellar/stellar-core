# Transactions
https://github.com/stellar/stellar-core/tree/master/src/transactions
Anything that changes the ledger is called a Transaction. Transactions have an aribtrary list of operations. 
Transactions have a list of signatures. (see multisig.md) The signatures are for the accounts used in the operations.



## All transactions have
- Source Account
- Max Fee they are willing to pay
- Sequence Number
- min ledger it is valid in (optional)
- max ledger it is valid in (optional)
- list of signatures
- list of operations




## Supported Operations
- Payment
	- Source Account (optional)
	- Destination Account
	- Amount
	- Path 
	- Meta Info
	- Source Meta
- Create Offer
	- Source Account (optional)
	- OfferID (optional)
- Cancel Offer
- AdjustAccount
- AccountMerge
- SetTrust
- Inflation


## Later:
- StoreData
- RemoveData


When a server receives a transaction it should send it on to all of its peers. 

## Rules for what txs are not flooded
- malformed
- sum signature weight not above the necessary threshold for each operation
- source account doesn't have enough balance to pay the fee for all the operations in this set (above the minimum balance)
- sequence number doesn't extend the locally accumulated set of transactions (or if empty, doesn't match the source account sequence number)
- duplicate signatures
- signatures that don't match any of the signers on the accounts used in the operations.







